"""OAuth/OIDC resource-server gateway for memgraph.

This ASGI app exposes:

* `/mcp` as MCP streamable-http, backed by the same tools as server.py.
* `/v1/*` as an authenticated proxy to the local memgraphd HTTP listener.

Token issuance, login, consent, and client registration belong to an external
OIDC provider. This process only validates access tokens and scopes.
"""

from __future__ import annotations

import asyncio
import json
import os
import time
import urllib.request
from dataclasses import dataclass
from typing import Any

import httpx
import jwt
from mcp.server.auth.provider import AccessToken, TokenVerifier
from mcp.server.auth.settings import AuthSettings
from starlette.applications import Starlette
from starlette.datastructures import Headers
from starlette.requests import Request
from starlette.responses import JSONResponse, Response
from starlette.routing import Mount, Route

from server import ADMIN_SCOPE, READ_SCOPE, WRITE_SCOPE, create_mcp


DEFAULT_UPSTREAM = "http://127.0.0.1:9977"
DEFAULT_REQUIRED_SCOPES = [READ_SCOPE]
HOP_BY_HOP_HEADERS = {
    "connection",
    "keep-alive",
    "proxy-authenticate",
    "proxy-authorization",
    "te",
    "trailers",
    "transfer-encoding",
    "upgrade",
    "host",
}


def _env_list(name: str, default: list[str]) -> list[str]:
    raw = os.environ.get(name)
    if raw is None or raw.strip() == "":
        return default
    return [part for part in raw.replace(",", " ").split() if part]


def _required_env(name: str) -> str:
    value = os.environ.get(name)
    if not value:
        raise RuntimeError(f"{name} is required")
    return value.rstrip("/")


@dataclass(frozen=True)
class GatewaySettings:
    issuer_url: str
    resource_server_url: str
    audience: str
    required_scopes: list[str]
    upstream_http: str
    jwks_cache_seconds: int

    @classmethod
    def from_env(cls) -> "GatewaySettings":
        return cls(
            issuer_url=_required_env("MEMGRAPH_OAUTH_ISSUER_URL"),
            resource_server_url=_required_env("MEMGRAPH_OAUTH_RESOURCE_SERVER_URL"),
            audience=_required_env("MEMGRAPH_OAUTH_AUDIENCE"),
            required_scopes=_env_list("MEMGRAPH_OAUTH_REQUIRED_SCOPES", DEFAULT_REQUIRED_SCOPES),
            upstream_http=os.environ.get("MEMGRAPH_UPSTREAM_HTTP", DEFAULT_UPSTREAM).rstrip("/"),
            jwks_cache_seconds=int(os.environ.get("MEMGRAPH_OAUTH_JWKS_CACHE_SECONDS", "300")),
        )


class OidcTokenVerifier(TokenVerifier):
    """Validate JWT access tokens using OIDC discovery and JWKS."""

    def __init__(
        self,
        issuer_url: str,
        audience: str,
        jwks_cache_seconds: int = 300,
        jwks_uri: str | None = None,
    ) -> None:
        self.issuer_url = issuer_url.rstrip("/")
        self.audience = audience
        self.jwks_cache_seconds = jwks_cache_seconds
        self._jwks_uri = jwks_uri
        self._jwk_client: jwt.PyJWKClient | None = None

    async def verify_token(self, token: str) -> AccessToken | None:
        try:
            claims = await asyncio.to_thread(self._decode_token, token)
        except jwt.PyJWTError:
            return None
        except (OSError, ValueError, KeyError, json.JSONDecodeError):
            return None

        scopes = _scopes_from_claims(claims)
        client_id = (
            claims.get("client_id")
            or claims.get("azp")
            or claims.get("sub")
            or "unknown"
        )
        expires_at = claims.get("exp")
        return AccessToken(
            token=token,
            client_id=str(client_id),
            scopes=scopes,
            expires_at=int(expires_at) if expires_at is not None else None,
            resource=self.audience,
        )

    def _decode_token(self, token: str) -> dict[str, Any]:
        client = self._get_jwk_client()
        signing_key = client.get_signing_key_from_jwt(token)
        return jwt.decode(
            token,
            signing_key.key,
            algorithms=["RS256", "RS384", "RS512", "ES256", "ES384", "ES512"],
            audience=self.audience,
            issuer=self.issuer_url,
            options={"require": ["exp", "iss", "aud"]},
        )

    def _get_jwk_client(self) -> jwt.PyJWKClient:
        if self._jwk_client is None:
            jwks_uri = self._jwks_uri or self._discover_jwks_uri()
            self._jwk_client = jwt.PyJWKClient(
                jwks_uri,
                cache_jwk_set=True,
                lifespan=self.jwks_cache_seconds,
            )
        return self._jwk_client

    def _discover_jwks_uri(self) -> str:
        url = f"{self.issuer_url}/.well-known/openid-configuration"
        with urllib.request.urlopen(url, timeout=10) as resp:
            metadata = json.loads(resp.read().decode("utf-8"))
        jwks_uri = metadata.get("jwks_uri")
        if not isinstance(jwks_uri, str) or not jwks_uri:
            raise ValueError("OIDC discovery document has no jwks_uri")
        return jwks_uri


def _scopes_from_claims(claims: dict[str, Any]) -> list[str]:
    scopes: set[str] = set()
    raw_scope = claims.get("scope")
    if isinstance(raw_scope, str):
        scopes.update(raw_scope.split())
    raw_scp = claims.get("scp")
    if isinstance(raw_scp, str):
        scopes.update(raw_scp.split())
    elif isinstance(raw_scp, list):
        scopes.update(str(scope) for scope in raw_scp)
    return sorted(scopes)


def scope_for_v1(method: str, path: str) -> str | None:
    if path == "/v1/healthz":
        return None
    if method == "POST" and path == "/v1/insert":
        return WRITE_SCOPE
    if method == "DELETE" and path.startswith("/v1/nodes/"):
        return ADMIN_SCOPE
    if method == "GET":
        if path in {
            "/v1/match",
            "/v1/search",
            "/v1/explore",
            "/v1/classify",
            "/v1/view",
        }:
            return READ_SCOPE
        if path.startswith("/v1/nodes/"):
            return READ_SCOPE
    return READ_SCOPE


def _bearer_token(headers: Headers) -> str | None:
    auth = headers.get("authorization")
    if not auth or not auth.lower().startswith("bearer "):
        return None
    token = auth[7:].strip()
    return token or None


async def _authorize_v1(
    request: Request,
    verifier: TokenVerifier,
) -> tuple[AccessToken | None, Response | None]:
    required_scope = scope_for_v1(request.method, request.url.path)
    if required_scope is None:
        return None, None

    raw_token = _bearer_token(request.headers)
    if raw_token is None:
        return None, _auth_error(401, "invalid_token", "Authentication required")
    token = await verifier.verify_token(raw_token)
    if token is None or (token.expires_at and token.expires_at < int(time.time())):
        return None, _auth_error(401, "invalid_token", "Invalid or expired token")
    if required_scope not in token.scopes:
        return token, _auth_error(403, "insufficient_scope", f"Required scope: {required_scope}")
    return token, None


def _auth_error(status: int, error: str, description: str) -> JSONResponse:
    return JSONResponse(
        {"error": error, "error_description": description},
        status_code=status,
        headers={"WWW-Authenticate": f'Bearer error="{error}", error_description="{description}"'},
    )


def _forward_headers(headers: Headers) -> dict[str, str]:
    return {
        key: value
        for key, value in headers.items()
        if key.lower() not in HOP_BY_HOP_HEADERS and key.lower() != "authorization"
    }


def _response_headers(headers: httpx.Headers) -> dict[str, str]:
    return {
        key: value
        for key, value in headers.items()
        if key.lower() not in HOP_BY_HOP_HEADERS and key.lower() not in {"content-length", "content-encoding"}
    }


async def proxy_v1(request: Request) -> Response:
    verifier: TokenVerifier = request.app.state.token_verifier
    _, auth_error = await _authorize_v1(request, verifier)
    if auth_error is not None:
        return auth_error

    upstream = request.app.state.upstream_http
    target = f"{upstream}{request.url.path}"
    if request.url.query:
        target = f"{target}?{request.url.query}"

    body = await request.body()
    async with httpx.AsyncClient(timeout=120.0) as client:
        upstream_resp = await client.request(
            request.method,
            target,
            headers=_forward_headers(request.headers),
            content=body,
        )

    return Response(
        upstream_resp.content,
        status_code=upstream_resp.status_code,
        headers=_response_headers(upstream_resp.headers),
        media_type=upstream_resp.headers.get("content-type"),
    )


def create_app(
    settings: GatewaySettings | None = None,
    token_verifier: TokenVerifier | None = None,
) -> Starlette:
    settings = settings or GatewaySettings.from_env()
    verifier = token_verifier or OidcTokenVerifier(
        settings.issuer_url,
        settings.audience,
        settings.jwks_cache_seconds,
    )
    auth = AuthSettings(
        issuer_url=settings.issuer_url,
        resource_server_url=settings.resource_server_url,
        required_scopes=settings.required_scopes,
    )
    mcp = create_mcp(
        token_verifier=verifier,
        auth=auth,
        streamable_http_path="/mcp",
        stateless_http=True,
        json_response=True,
    )
    mcp_app = mcp.streamable_http_app()
    app = Starlette(
        routes=[
            Route("/v1/{path:path}", proxy_v1, methods=["GET", "POST", "DELETE", "OPTIONS"]),
            Mount("/", app=mcp_app),
        ]
    )
    app.state.token_verifier = verifier
    app.state.upstream_http = settings.upstream_http
    return app


app = create_app()
