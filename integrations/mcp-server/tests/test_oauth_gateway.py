from __future__ import annotations

import importlib
import sys
import time
from pathlib import Path

import httpx
import pytest
from cryptography.hazmat.primitives.asymmetric import rsa
from mcp.server.auth.provider import AccessToken
from starlette.testclient import TestClient

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))


@pytest.fixture()
def gateway(monkeypatch):
    monkeypatch.setenv("GRAFT_OAUTH_ISSUER_URL", "https://issuer.example")
    monkeypatch.setenv("GRAFT_OAUTH_RESOURCE_SERVER_URL", "https://graft.example/mcp")
    monkeypatch.setenv("GRAFT_OAUTH_AUDIENCE", "https://graft.example")
    module = importlib.import_module("oauth_gateway")
    return module


class FakeVerifier:
    async def verify_token(self, token: str):
        if token == "read":
            return AccessToken(token=token, client_id="test", scopes=["graft:read"], expires_at=int(time.time()) + 60)
        if token == "write":
            return AccessToken(token=token, client_id="test", scopes=["graft:write"], expires_at=int(time.time()) + 60)
        if token == "expired":
            return AccessToken(token=token, client_id="test", scopes=["graft:read"], expires_at=int(time.time()) - 1)
        return None


class FakeAsyncClient:
    def __init__(self, *args, **kwargs):
        pass

    async def __aenter__(self):
        return self

    async def __aexit__(self, *exc):
        return None

    async def request(self, method, url, headers=None, content=None):
        return httpx.Response(
            200,
            json={"method": method, "url": url, "content": (content or b"").decode()},
            headers={"content-type": "application/json"},
        )


def make_app(gateway, verifier=None):
    settings = gateway.GatewaySettings(
        issuer_url="https://issuer.example",
        resource_server_url="https://graft.example/mcp",
        audience="https://graft.example",
        required_scopes=["graft:read"],
        upstream_http="http://127.0.0.1:9977",
        jwks_cache_seconds=300,
    )
    return gateway.create_app(settings=settings, token_verifier=verifier or FakeVerifier())


def test_read_token_can_access_v1_search(gateway, monkeypatch):
    monkeypatch.setattr(gateway.httpx, "AsyncClient", FakeAsyncClient)
    client = TestClient(make_app(gateway))

    resp = client.get("/v1/search?text=oauth", headers={"Authorization": "Bearer read"})

    assert resp.status_code == 200
    assert resp.json()["url"] == "http://127.0.0.1:9977/v1/search?text=oauth"


def test_missing_malformed_and_expired_tokens_receive_401(gateway):
    client = TestClient(make_app(gateway))

    assert client.get("/v1/search?text=x").status_code == 401
    assert client.get("/v1/search?text=x", headers={"Authorization": "Bearer nope"}).status_code == 401
    assert client.get("/v1/search?text=x", headers={"Authorization": "Bearer expired"}).status_code == 401


def test_token_without_required_scope_receives_403(gateway):
    client = TestClient(make_app(gateway))

    resp = client.get("/v1/search?text=x", headers={"Authorization": "Bearer write"})

    assert resp.status_code == 403
    assert resp.json()["error"] == "insufficient_scope"


def test_mcp_exposes_protected_resource_metadata(gateway):
    client = TestClient(make_app(gateway))

    resp = client.get("/.well-known/oauth-protected-resource/mcp")

    assert resp.status_code == 200
    body = resp.json()
    assert body["resource"] == "https://graft.example/mcp"
    assert body["authorization_servers"] == ["https://issuer.example/"]
    assert "graft:read" in body["scopes_supported"]


def test_oidc_verifier_rejects_wrong_issuer_and_audience(gateway, monkeypatch):
    private_key = rsa.generate_private_key(public_exponent=65537, key_size=2048)
    verifier = gateway.OidcTokenVerifier(
        "https://issuer.example",
        "https://graft.example",
        jwks_uri="https://issuer.example/jwks",
    )

    class FakeKeyClient:
        def get_signing_key_from_jwt(self, token):
            class SigningKey:
                key = private_key.public_key()

            return SigningKey()

    monkeypatch.setattr(verifier, "_jwk_client", FakeKeyClient())

    now = int(time.time())
    good = gateway.jwt.encode(
        {
            "iss": "https://issuer.example",
            "aud": "https://graft.example",
            "exp": now + 60,
            "sub": "client",
            "scope": "graft:read",
        },
        private_key,
        algorithm="RS256",
    )
    wrong_issuer = gateway.jwt.encode(
        {
            "iss": "https://other.example",
            "aud": "https://graft.example",
            "exp": now + 60,
            "sub": "client",
            "scope": "graft:read",
        },
        private_key,
        algorithm="RS256",
    )
    wrong_audience = gateway.jwt.encode(
        {
            "iss": "https://issuer.example",
            "aud": "https://other.example",
            "exp": now + 60,
            "sub": "client",
            "scope": "graft:read",
        },
        private_key,
        algorithm="RS256",
    )

    import asyncio

    assert asyncio.run(verifier.verify_token(good)).scopes == ["graft:read"]
    assert asyncio.run(verifier.verify_token(wrong_issuer)) is None
    assert asyncio.run(verifier.verify_token(wrong_audience)) is None
