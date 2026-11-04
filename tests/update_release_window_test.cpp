// `agentty update` must never offer an update it cannot perform (#119).
//
// The release workflow streams: the release page and source tarball are
// published first, and each platform's binary lands over the next ~35-80
// minutes. The client used to read /releases/latest and trust the version
// alone, so for up to an hour after every release it said "v0.9.46
// available" and then failed with a bare HTTP 404.
//
// choose_release is the pure decision: given GitHub's /releases list, the
// running version and this platform's asset name, what to offer. These
// fixtures use the real response shape (tag_name, draft, prerelease,
// html_url, assets[].name/state/size/digest).
#include "agtest.hpp"

#include "agentty/util/update.hpp"

#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace up = agentty::update;
using nlohmann::json;

namespace {

// GitHub /releases?per_page=5, trimmed to the fields the client reads.
constexpr const char* kIssue119Releases = R"JSON([{"tag_name":"v0.9.46","draft":false,"prerelease":false,"html_url":"https://github.com/agentty-org/agentty/releases/tag/v0.9.46","assets":[{"name":"aarch64.diag.txt","state":"uploaded","size":1531,"digest":"sha256:32026f34e0bbe1a7803962f2b37a86d9bb1ccc1663f28ed730634a362e3c8ade"},{"name":"agentty-0.9.46-1.aarch64.rpm","state":"uploaded","size":9377479,"digest":"sha256:490075a9a98e05f1193d5a7241e342e4c8b089048884d04469066e441dac8ed5"},{"name":"agentty-0.9.46.tar.gz","state":"uploaded","size":26578019,"digest":"sha256:0a7b7cd780db78dd7eab7f044ee6fe80b8a0f48c7e0d8d1acce9526618125ace"},{"name":"agentty-linux-aarch64","state":"uploaded","size":22382192,"digest":"sha256:151f40d4c669d729f894bfb494a9f3cf5817f8ff3573a35ae768f6594426ee83"},{"name":"agentty-macos-arm64","state":"uploaded","size":43228296,"digest":"sha256:31e76c4eb286defd9a0f1a53a0d7acba0b6fb15fd24e69ef36b2378ca9c8e558"},{"name":"agentty-macos-x86_64","state":"uploaded","size":51864696,"digest":"sha256:8aab037240c6e18edeba0c1048bc227c0f32a3010e074e49b15f32bc0727d674"},{"name":"agentty-windows-x86_64.exe","state":"uploaded","size":27057152,"digest":"sha256:a9901fb3817cb01123d381229273fdc76801b2896a812756b467d6cde1eb890a"},{"name":"agentty-windows-x86_64.msi","state":"uploaded","size":8269824,"digest":"sha256:99e1bfdfef1bc788e0311812c92f4f5b2f5af417ba67d46c790e1fbc14443fb5"},{"name":"agentty_0.9.46_arm64.deb","state":"uploaded","size":9412414,"digest":"sha256:3f68cf3b802beac2c7c6916199c880d8c25ca5f5ccd150e1bb50947b7d28f372"}]},{"tag_name":"v0.9.45","draft":false,"prerelease":false,"html_url":"https://github.com/agentty-org/agentty/releases/tag/v0.9.45","assets":[{"name":"aarch64.diag.txt","state":"uploaded","size":1531,"digest":"sha256:bbaf1ad768cb4badb2829b7c8f16570ab4e77df2cefb96a696f54271ff79bb66"},{"name":"agentty-0.9.45-1.aarch64.rpm","state":"uploaded","size":9350152,"digest":"sha256:3d264b418917f88da6682c707df88f2dd007f260f92813ac7ffa001400d4ab23"},{"name":"agentty-0.9.45-1.x86_64.rpm","state":"uploaded","size":9182572,"digest":"sha256:c9b8c126bdbb4a9665acfd49be3b04554eae6c48eb7c64aa7430df3040496dde"},{"name":"agentty-0.9.45.ebuild","state":"uploaded","size":1581,"digest":"sha256:2a189945c2f035b9ceaff5d8fda6bd8a7088b3ad088e2ab8a13a4a179d9b1b3b"},{"name":"agentty-0.9.45.tar.gz","state":"uploaded","size":26491053,"digest":"sha256:ac7adaaac8cefec1f227229dd32ffb294e3e527a8d25f6288c32a50842b8ae8b"},{"name":"agentty-bin-0.9.45-1-x86_64.pkg.tar.zst","state":"uploaded","size":7199128,"digest":"sha256:2a417651c0126c200d13a28daec4b01df6f2dbb4b76f3e5a06fcb110647d388a"},{"name":"agentty-linux-aarch64","state":"uploaded","size":22316648,"digest":"sha256:baec022ae0399a58e10fe9a47c1b287090a17029344c7da2f286a37085ed24c6"},{"name":"agentty-linux-i686","state":"uploaded","size":24816764,"digest":"sha256:5d38273462f05f3cfaa510ce4f2c5ba2c440d1cdddae9756cded87d52daf0c21"},{"name":"agentty-linux-x86_64","state":"uploaded","size":23891112,"digest":"sha256:af1291ee557b607240c33b5c7aec54473057b5ccadda533efb71fb79338d9c8b"},{"name":"agentty-macos-arm64","state":"uploaded","size":43127016,"digest":"sha256:7d8a2e36a6c71eb0d2715d755e76cb13409dfc6cf4481f9bdd38169de99f09d0"},{"name":"agentty-macos-x86_64","state":"uploaded","size":51721336,"digest":"sha256:c2eadac1e716f71892ce00091c7968466b8b3454875b957926ed961cc7c43e44"},{"name":"agentty-windows-x86_64.exe","state":"uploaded","size":27017216,"digest":"sha256:3ab7a41d8c8ea007e9557be9781630c78b2205bd52394adaee74560fb6acbd3b"},{"name":"agentty-windows-x86_64.msi","state":"uploaded","size":8257536,"digest":"sha256:7e28df35add72ba3c5a9c46bc796e67670b9773e942bcce7eb0f99af06c140a4"},{"name":"agentty_0.9.45_amd64.deb","state":"uploaded","size":9247772,"digest":"sha256:50cd8ad83d03b2058689c14e0fb46c826c36a198d0c60970c459b1cb74f0eee5"},{"name":"agentty_0.9.45_arm64.deb","state":"uploaded","size":9383748,"digest":"sha256:3940c2ab7bb719981ab7801d1ebe703ea71772cfefc6ab5e0ada29427d59993a"},{"name":"default.nix","state":"uploaded","size":2156,"digest":"sha256:48da1c4eb695d405bd29dc6c935696d7f73fb590666b54d0ff07761bc8800bfc"},{"name":"SHA256SUMS","state":"uploaded","size":1267,"digest":"sha256:c0a12057b5eeffe7ee06527223fb8d143b6e34de9b7dd361b479783f30f1548c"},{"name":"snapcraft.yaml","state":"uploaded","size":1716,"digest":"sha256:ed0aa05545da7678c9965b9145680c7df6786295577bffd8fc364d4864348c48"}]},{"tag_name":"v0.9.44","draft":false,"prerelease":false,"html_url":"https://github.com/agentty-org/agentty/releases/tag/v0.9.44","assets":[{"name":"aarch64.diag.txt","state":"uploaded","size":1531,"digest":"sha256:bbaf1ad768cb4badb2829b7c8f16570ab4e77df2cefb96a696f54271ff79bb66"},{"name":"agentty-0.9.44-1.aarch64.rpm","state":"uploaded","size":9348761,"digest":"sha256:36c8ff4c25225c462b5872876dfb4dcdf9c3dbb769f4907704f9b6003f7d8d24"},{"name":"agentty-0.9.44-1.x86_64.rpm","state":"uploaded","size":9180790,"digest":"sha256:99eb561e0d234068d600ef5cfa27493783a84034fc91713c3af9860a7393a2f1"},{"name":"agentty-0.9.44.ebuild","state":"uploaded","size":1581,"digest":"sha256:2a189945c2f035b9ceaff5d8fda6bd8a7088b3ad088e2ab8a13a4a179d9b1b3b"},{"name":"agentty-0.9.44.tar.gz","state":"uploaded","size":26488587,"digest":"sha256:3dea7c328f2ac94b2305fee58137af5155d6860122335877af7fd290a64b4057"},{"name":"agentty-bin-0.9.44-1-x86_64.pkg.tar.zst","state":"uploaded","size":7199439,"digest":"sha256:75998a9b2f1cdbefce31dfbecec0ee3a8cd25fb788319adba2d26442ec70f30f"},{"name":"agentty-linux-aarch64","state":"uploaded","size":22316648,"digest":"sha256:b4387abc52a2b8aac2553ba8be0e34efdd1d1d5c5dc631c7292d9bf536453bb5"},{"name":"agentty-linux-x86_64","state":"uploaded","size":23887016,"digest":"sha256:6b66fc3effe2d89a44a670134117aa09bfd5473d50e44d971ed02bec2b6c604e"},{"name":"agentty-windows-x86_64.exe","state":"uploaded","size":27010048,"digest":"sha256:3c33f42f1b9cf030fbf104dbd4d1a51285ee4b7798f81137d8c3470d23d5bf4c"},{"name":"agentty-windows-x86_64.msi","state":"uploaded","size":8253440,"digest":"sha256:e4970aa447dc0ce3785930d8dead93668273f4f6817ace5eef16ab42f76b7ae0"},{"name":"agentty_0.9.44_amd64.deb","state":"uploaded","size":9243900,"digest":"sha256:b3c7c6a07388e0d267979a651033c7f379e0cb51125e8197a5823f541f3c44e9"},{"name":"agentty_0.9.44_arm64.deb","state":"uploaded","size":9383778,"digest":"sha256:4ba74e42016f83b790871fccfc129c0a712a104705fa72b152a12c7712770b84"},{"name":"default.nix","state":"uploaded","size":2156,"digest":"sha256:2c389713bf1c63e07f80ce8d3907b15ab14cda1517c550a3fe7706fb95131e23"},{"name":"SHA256SUMS","state":"uploaded","size":1009,"digest":"sha256:538550a75e147d3854d3a7c0c61c6502bee3be590620728cf4154220234fa768"},{"name":"snapcraft.yaml","state":"uploaded","size":1716,"digest":"sha256:ea5d2980efd56f5e1b9864407a12402b08eef13ab526be91b029cb0465a59fe0"}]},{"tag_name":"v0.9.43","draft":false,"prerelease":false,"html_url":"https://github.com/agentty-org/agentty/releases/tag/v0.9.43","assets":[{"name":"aarch64.diag.txt","state":"uploaded","size":1531,"digest":"sha256:367e7355441db5a94d2b77df757a4d71d1e417f00517a1b2e2ff6672fa33390e"},{"name":"agentty-0.9.43-1.aarch64.rpm","state":"uploaded","size":11861366,"digest":"sha256:aece90304dcedd29c969a85d70956b1e12eeef6d550be6c057ceb9200c9dc2fd"},{"name":"agentty-0.9.43-1.x86_64.rpm","state":"uploaded","size":11849648,"digest":"sha256:81a6bba81662768ab33a832f3fa67b53ae7723fe9aead5db5ad332dd57b53b6b"},{"name":"agentty-0.9.43.ebuild","state":"uploaded","size":1581,"digest":"sha256:2a189945c2f035b9ceaff5d8fda6bd8a7088b3ad088e2ab8a13a4a179d9b1b3b"},{"name":"agentty-0.9.43.tar.gz","state":"uploaded","size":26476139,"digest":"sha256:a1be410a8598f4a811e628c1cf836cc48770a8e40bb53d5039f6d5b9779511a3"},{"name":"agentty-bin-0.9.43-1-x86_64.pkg.tar.zst","state":"uploaded","size":8991514,"digest":"sha256:20362969cb9a1ac737c973767aef29f59e8cbaa33cd08d803b7de7f05d7162f6"},{"name":"agentty-linux-aarch64","state":"uploaded","size":27756168,"digest":"sha256:7277b3547ddd58003d5dfc658fc66585f9c76a7c7eb8ec25eaaa8358afe18ad8"},{"name":"agentty-linux-i686","state":"uploaded","size":31710332,"digest":"sha256:a3786a6b7af5351202d6d5cd28c25ed46154536c626605a90fe9883655db9534"},{"name":"agentty-linux-x86_64","state":"uploaded","size":30952616,"digest":"sha256:7e07daa577de4fb1149aaf48f8c341fc0d7d48fcbb2b50b727e12d3bde1bc851"},{"name":"agentty-macos-arm64","state":"uploaded","size":50911560,"digest":"sha256:75ed4328a74b2d41b9cb0e5cd2cbdb81572b657a61f598d4336d6d2f95bcbf77"},{"name":"agentty-macos-x86_64","state":"uploaded","size":59445800,"digest":"sha256:76922feaf62be71656f7b6639a90b6a90b031d0ece64014d74c561c35c495c92"},{"name":"agentty-windows-x86_64.exe","state":"uploaded","size":26833408,"digest":"sha256:eb7d1f38c5c55ae8cb53472d145f507ae874f946756a935bf599502d9eb559d9"},{"name":"agentty-windows-x86_64.msi","state":"uploaded","size":8249344,"digest":"sha256:80f89793749df9b406174f6b137eb6c95ffafb30798264ca82f7011edcebb741"},{"name":"agentty_0.9.43_amd64.deb","state":"uploaded","size":11939004,"digest":"sha256:a4bf2f784d44fb05f191dd9d5d6e46dc4a270c18a7f2629b4a7d13ceb6994bc0"},{"name":"agentty_0.9.43_arm64.deb","state":"uploaded","size":11892148,"digest":"sha256:810607d1e7d91f505d4324d28bd8bee8d30b4b43a7c423c992dff936c4e0cab6"},{"name":"default.nix","state":"uploaded","size":2156,"digest":"sha256:7acceb7a8fe90422b0eb3aa29444b4dbfd0f6a9b6841c78f34d184162fefcffc"},{"name":"SHA256SUMS","state":"uploaded","size":1267,"digest":"sha256:03301c58b6eb2431d4bd862913293192e3403de91998d2813646d4721b2ac48f"},{"name":"snapcraft.yaml","state":"uploaded","size":1716,"digest":"sha256:7e788b1f7ab6175813d8342d7748cca937d486b0f0c3b43620128e3e531b629c"}]},{"tag_name":"v0.9.42","draft":false,"prerelease":false,"html_url":"https://github.com/agentty-org/agentty/releases/tag/v0.9.42","assets":[{"name":"aarch64.diag.txt","state":"uploaded","size":1531,"digest":"sha256:4fe694fd593c4abce0cc3c51871a028e4206d3090b997120ac84b144747e2a52"},{"name":"agentty-0.9.42-1.aarch64.rpm","state":"uploaded","size":11816938,"digest":"sha256:8821a60cddccceeb0dda90b0ea491eb02ab104e68802d1534bd5208245f82326"},{"name":"agentty-0.9.42-1.x86_64.rpm","state":"uploaded","size":11791333,"digest":"sha256:ab4ca3d581b150595a1d8002030bb975912505503e1498994e4f0744fca884ca"},{"name":"agentty-0.9.42.ebuild","state":"uploaded","size":1581,"digest":"sha256:2a189945c2f035b9ceaff5d8fda6bd8a7088b3ad088e2ab8a13a4a179d9b1b3b"},{"name":"agentty-0.9.42.tar.gz","state":"uploaded","size":26453797,"digest":"sha256:31ec5eb6c2ee04d439b6a79e13e30453e38615190ec7885b20daf2b171089220"},{"name":"agentty-bin-0.9.42-1-x86_64.pkg.tar.zst","state":"uploaded","size":8972077,"digest":"sha256:5f8618fafdb9bd783e68dbbf72c3b9146934f28cbbe199810bcacf5092fe50a2"},{"name":"agentty-linux-aarch64","state":"uploaded","size":27625088,"digest":"sha256:31c4e56a3a2291a548cad700b9e8e98e77273758daf8c30c9c97c4f1eb7db675"},{"name":"agentty-linux-i686","state":"uploaded","size":31616124,"digest":"sha256:a9a533561a4297fa39395697b14f825223398d94edd7dd6900e613664adaca8c"},{"name":"agentty-linux-x86_64","state":"uploaded","size":30866568,"digest":"sha256:11acd839416ebdc0ac006dc89c67e6d932fb04e24296da121488b1e51eeef95e"},{"name":"agentty-macos-arm64","state":"uploaded","size":50672248,"digest":"sha256:f4683ddeb6a1b1441b72c9fd893d348265b436b2610fa7ac78c8937b0158ea4e"},{"name":"agentty-macos-x86_64","state":"uploaded","size":59041744,"digest":"sha256:7f372323d7a6546166dd096626b42bdacae0e759b60fc5fb5e11defed9828ddd"},{"name":"agentty-windows-x86_64.exe","state":"uploaded","size":26744320,"digest":"sha256:80edfaf92417347e205b9cb0bd42dbe0a804090d9b752c220edd2a01de4f7c41"},{"name":"agentty-windows-x86_64.msi","state":"uploaded","size":8216576,"digest":"sha256:8f0d92521aba9ae6e6173e85d748ca50986f5a34cc074cbcbebef848e9444017"},{"name":"agentty_0.9.42_amd64.deb","state":"uploaded","size":11879008,"digest":"sha256:3835af0afbe3eb3e3b531a6184a58fde382a61ad7b3690c442e96bef1b58b673"},{"name":"agentty_0.9.42_arm64.deb","state":"uploaded","size":11853198,"digest":"sha256:43597779c319e2726df7895554a1e67fba66d05db5b1dc745a78408d961fae73"},{"name":"default.nix","state":"uploaded","size":2156,"digest":"sha256:2ba9f58cef52d9a7fb47f618f9daa00844111f63e340ad9a242632c2db12c1e8"},{"name":"SHA256SUMS","state":"uploaded","size":1514,"digest":"sha256:ad8c43fcde7aa98e957bd513a29e4b1d9d6b989532734001f6e1e227993ac570"},{"name":"snapcraft.yaml","state":"uploaded","size":1716,"digest":"sha256:1bcf42fa1544abf7cb83aa5f279aa93d99f7909cb79d478648fe9d18df63aa6c"}]}])JSON";

constexpr const char* kAsset = "agentty-linux-x86_64";
constexpr std::int64_t kBin = 23'000'000;   // a real binary is ~22 MB

json asset(const std::string& name, std::int64_t size = kBin,
           const std::string& state = "uploaded",
           const std::string& sha = "ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12cd34ef56ab12") {
    return {{"name", name}, {"size", size}, {"state", state}, {"digest", "sha256:" + sha}};
}

json release(const std::string& tag, std::vector<json> assets, bool draft = false,
             bool pre = false) {
    return {{"tag_name", tag}, {"draft", draft}, {"prerelease", pre},
            {"html_url", "https://github.com/agentty-org/agentty/releases/tag/" + tag},
            {"assets", assets}};
}

up::CheckResult choose(const json& releases, const std::string& current = "0.9.45") {
    return up::choose_release(releases.dump(), current, kAsset);
}

}  // namespace

TEST_CASE("update: the exact #119 window -- newest release has no binary yet") {
    // v0.9.46 published ~60 min ago: only the source tarball is up.
    const json rs = json::array({
        release("v0.9.46", {asset("agentty-0.9.46.tar.gz", 26'000'000)}),
        release("v0.9.45", {asset(kAsset), asset("agentty-macos-arm64")}),
    });
    const auto c = choose(rs);
    CHECK(c.error.empty());
    CHECK(c.latest == "0.9.46");
    // nothing installable is NEWER than what we run: no offer, no chip.
    CHECK_FALSE(c.update_available);
    CHECK(c.target.empty());
    // ...but the client knows a release is on its way, so it can say so.
    CHECK(c.pending);
}

TEST_CASE("update: once the binary lands, it is offered with its digest") {
    const std::string sha = "151f40d4c669d729f894bfb494a9f3cf5817f8ff3573a35ae768f6594426ee83";
    const json rs = json::array({
        release("v0.9.46", {asset("agentty-0.9.46.tar.gz", 26'000'000),
                            asset(kAsset, kBin, "uploaded", sha)}),
        release("v0.9.45", {asset(kAsset)}),
    });
    const auto c = choose(rs);
    CHECK(c.update_available);
    CHECK(c.target == "0.9.46");
    CHECK_FALSE(c.pending);
    CHECK(c.asset_sha256 == sha);
    CHECK(c.url.find("v0.9.46") != std::string::npos);
}

TEST_CASE("update: a user two versions behind gets the newest INSTALLABLE one") {
    // On 0.9.44 while 0.9.46 builds: 0.9.45 is a real update right now.
    const json rs = json::array({
        release("v0.9.46", {asset("agentty-0.9.46.tar.gz", 26'000'000)}),
        release("v0.9.45", {asset(kAsset)}),
        release("v0.9.44", {asset(kAsset)}),
    });
    const auto c = choose(rs, "0.9.44");
    CHECK(c.update_available);
    CHECK(c.target == "0.9.45");
    CHECK(c.latest == "0.9.46");
    CHECK(c.pending);   // and 0.9.46 is coming
    CHECK(c.url.find("v0.9.45") != std::string::npos);   // link to what we'd install
}

TEST_CASE("update: a half-uploaded or placeholder asset does not count") {
    // `--clobber` mid-upload: GitHub reports state "starter" until it's done.
    json rs = json::array({release("v0.9.46", {asset(kAsset, kBin, "starter")})});
    CHECK_FALSE(choose(rs).update_available);
    CHECK(choose(rs).pending);
    // a stub smaller than any real binary.
    rs = json::array({release("v0.9.46", {asset(kAsset, 4096)})});
    CHECK_FALSE(choose(rs).update_available);
}

TEST_CASE("update: another platform's binary does not make ours installable") {
    // the 0.9.44 precedent: linux shipped, macOS didn't.
    const json rs = json::array({
        release("v0.9.46", {asset("agentty-macos-arm64"), asset("agentty-linux-aarch64")}),
    });
    const auto c = choose(rs);
    CHECK_FALSE(c.update_available);
    CHECK(c.pending);
}

TEST_CASE("update: drafts and prereleases are never offered, nor counted as latest") {
    const json rs = json::array({
        release("v0.9.48", {asset(kAsset)}, /*draft=*/true),
        release("v0.9.47", {asset(kAsset)}, false, /*pre=*/true),
        release("v0.9.45", {asset(kAsset)}),
    });
    const auto c = choose(rs);
    CHECK(c.latest == "0.9.45");
    CHECK_FALSE(c.update_available);
    CHECK_FALSE(c.pending);
}

TEST_CASE("update: the REAL #119 response, captured mid-release") {
    // GitHub's actual /releases?per_page=5 body, captured while v0.9.46 was
    // still building: linux-aarch64, macOS and Windows were up, linux-x86_64
    // (the slowest lane, the reporter's platform) was not.
    const std::string body = kIssue119Releases;

    // the reporter: 0.9.45 on linux x86_64. Before the fix: "v0.9.46
    // available", then HTTP 404. Now: no offer, and a clear "building".
    auto c = up::choose_release(body, "0.9.45", "agentty-linux-x86_64");
    CHECK(c.error.empty());
    CHECK(c.latest == "0.9.46");
    CHECK_FALSE(c.update_available);
    CHECK(c.pending);

    // the same moment on platforms whose binary HAD landed: offered, with
    // GitHub's digest for that exact file.
    for (const char* a : {"agentty-linux-aarch64", "agentty-macos-arm64",
                          "agentty-windows-x86_64.exe"}) {
        CAPTURE(a);
        c = up::choose_release(body, "0.9.45", a);
        CHECK(c.update_available);
        CHECK(c.target == "0.9.46");
        CHECK_FALSE(c.pending);
        CHECK(c.asset_sha256.size() == 64);
    }
}

TEST_CASE("update: up to date is quiet") {
    const json rs = json::array({release("v0.9.45", {asset(kAsset)})});
    const auto c = choose(rs);
    CHECK_FALSE(c.update_available);
    CHECK_FALSE(c.pending);
    CHECK(c.latest == "0.9.45");
}

TEST_CASE("update: order of the list does not matter, and junk is survivable") {
    const json rs = json::array({
        release("v0.9.44", {asset(kAsset)}),
        "not an object",
        release("", {asset(kAsset)}),
        release("v0.9.46", {asset(kAsset)}),
        release("v0.9.45", {asset(kAsset)}),
    });
    const auto c = choose(rs, "0.9.44");
    CHECK(c.target == "0.9.46");
    CHECK(c.latest == "0.9.46");
    // and a body that isn't JSON is an error, not a crash or an offer.
    const auto bad = up::choose_release("<html>rate limited</html>", "0.9.45", kAsset);
    CHECK_FALSE(bad.error.empty());
    CHECK_FALSE(bad.update_available);
}

TEST_CASE("update: an asset without a digest still installs, just unverified") {
    json a = asset(kAsset);
    a.erase("digest");   // older releases predate GitHub asset digests
    const json rs = json::array({release("v0.9.46", {a})});
    const auto c = choose(rs);
    CHECK(c.update_available);
    CHECK(c.asset_sha256.empty());
}
