class Graft < Formula
  desc "Persistent graph memory for AI agents"
  homepage "https://github.com/AEndrix03/Graft"
  license "Apache-2.0"
  head "https://github.com/AEndrix03/Graft.git", branch: "master"

  livecheck do
    skip "no tagged stable release yet; install with --HEAD"
  end

  depends_on "cmake" => :build
  depends_on "git" => :build
  depends_on "pkgconf" => :build
  depends_on "libyaml"
  depends_on "sqlite"

  resource "blake3" do
    url "https://github.com/BLAKE3-team/BLAKE3.git",
        branch:   "master",
        revision: "f3913d953128661319b6b57b1c001a3b9c2d526e"
  end

  resource "llama.cpp" do
    url "https://github.com/ggerganov/llama.cpp.git",
        tag:      "b9037",
        revision: "bbeb89d76c41bc250f16e4a6fefcc9b530d6e3f3"
  end

  resource "mpack" do
    url "https://github.com/ludocode/mpack.git",
        branch:   "develop",
        revision: "a2d720270329be5d2179cd71aad6c8014d1cc555"
  end

  resource "sqlite-vec" do
    url "https://github.com/asg017/sqlite-vec.git",
        branch:   "main",
        revision: "5778fecfebaddafc23b69a3a4b91a8ee80e37a92"
  end

  resource "bge-m3" do
    url "https://huggingface.co/lm-kit/bge-m3-gguf/resolve/9379ce497e8814b200f2dc0d18eb4045426dcb8c/bge-m3-Q8_0.gguf"
    sha256 "c98c7c907bb3ab8c3be4c8ad827cc934c7c1a7e014d1c999e0c45c8027c158bb"
  end

  def stage_resource(name, destination)
    rm_rf destination
    destination.mkpath
    resource(name).stage destination
  end

  def install
    stage_resource "blake3", buildpath/"third_party/BLAKE3"
    stage_resource "llama.cpp", buildpath/"third_party/llama.cpp"
    stage_resource "mpack", buildpath/"third_party/mpack"
    stage_resource "sqlite-vec", buildpath/"third_party/sqlite-vec"

    rpath_token = OS.mac? ? "@loader_path" : "$ORIGIN"
    llama_args = %W[
      -S third_party/llama.cpp
      -B third_party/llama.cpp/build
      -DBUILD_SHARED_LIBS=ON
      -DGGML_NATIVE=OFF
      -DLLAMA_CURL=OFF
      -DLLAMA_BUILD_SERVER=OFF
      -DLLAMA_BUILD_TOOLS=OFF
      -DLLAMA_BUILD_EXAMPLES=OFF
      -DLLAMA_BUILD_TESTS=OFF
      -DLLAMA_BUILD_COMMON=OFF
      -DCMAKE_BUILD_TYPE=Release
    ]
    llama_args << "-DCMAKE_BUILD_RPATH=#{rpath_token}"
    system "cmake", *llama_args
    system "cmake", "--build", "third_party/llama.cpp/build", "--parallel"

    graft_args = std_cmake_args + %W[
      -DCMAKE_BUILD_TYPE=Release
      -DCMAKE_BUILD_RPATH=#{rpath_token}
      -DGRAFT_BUILD_TESTS=OFF
    ]
    system "cmake", "-S", ".", "-B", "build", *graft_args
    system "cmake", "--build", "build", "--parallel"

    bin.install "build/graft", "build/graftd"
    shared_globs = if OS.mac?
      %w[
        third_party/llama.cpp/build/bin/*.dylib
        third_party/llama.cpp/build/src/*.dylib
        third_party/llama.cpp/build/ggml/src/**/*.dylib
      ]
    else
      %w[
        third_party/llama.cpp/build/bin/*.so*
        third_party/llama.cpp/build/src/*.so*
        third_party/llama.cpp/build/ggml/src/**/*.so*
      ]
    end
    bin.install Dir[*shared_globs]

    (pkgshare/"models").mkpath
    resource("bge-m3").stage do
      (pkgshare/"models").install "bge-m3-Q8_0.gguf" => "bge-m3.gguf"
    end

    config = (buildpath/"config.example.yaml").read
    config.gsub!(/^[[:space:]]*model_path:.*$/, "  model_path: \"#{pkgshare}/models/bge-m3.gguf\"")
    config.gsub!(/^[[:space:]]*viewer_path:.*$/, "  viewer_path: \"#{pkgshare}/viewer\"")
    (prefix/"config.example.yaml").write config

    if (buildpath/"viewer/dist").directory?
      pkgshare.install "viewer/dist" => "viewer"
    end

    (pkgshare/"integrations").install "integrations/standard" => "standard"
  end

  def caveats
    <<~EOS
      Graft has no tagged stable release yet; install it with:
        brew install --HEAD graft

      The default Homebrew config is:
        #{prefix}/config.example.yaml

      User data still lives under ~/.graft by default. To customize runtime
      settings, copy the config once:
        mkdir -p ~/.graft
        cp #{prefix}/config.example.yaml ~/.graft/config.yaml

      Then edit ~/.graft/config.yaml and run:
        graft stats
    EOS
  end

  test do
    assert_match "profile", shell_output("#{bin}/graft profile list")
  end
end
