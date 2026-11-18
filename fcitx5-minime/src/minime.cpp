// fcitx5-minime — MinIME 的 fcitx5 前端：用微信输入法引擎打字（异步版）
//
// 链路（优先）: fcitx5 --Unix socket--> minime-engined（原生进程装载 libwxhld）
//   socket 路径 $XDG_RUNTIME_DIR/minime/engine.sock
// 链路（回退）: fcitx5 --TCP 127.0.0.1:17890--> bridge.exe(Wine) --命名管道--> wetype_server --> libwxhld
// 两条链路说同一个协议，帧格式相同。
//
// 协议与编码见 docs/architecture.md。要点:
//  - 会话绑定 TCP/管道连接；bridge 先发 "PID:<n>"，app_info.process_id 必须用它
//  - 响应异步到达 -> 用 fcitx5 事件循环的 IO 事件读取，keyEvent 不阻塞
//  - SELECT_CANDIDATE 必须带 commit=CandidateWord{value,id}
#include <fcitx-config/configuration.h>
#include <fcitx-config/iniparser.h>
#include <fcitx-config/option.h>
#include <fcitx/addonfactory.h>
#include <fcitx/addoninstance.h>
#include <fcitx/addonmanager.h>
#include <fcitx/candidateaction.h>
#include <fcitx/candidatelist.h>
#include <fcitx/event.h>
#include <fcitx/inputcontext.h>
#include <fcitx/inputcontextmanager.h>
#include <fcitx/inputcontextproperty.h>
#include <fcitx/inputmethodengine.h>
#include <fcitx/inputpanel.h>
#include <fcitx/instance.h>
#include <fcitx-utils/capabilityflags.h>
#include <fcitx-utils/event.h>
#include <fcitx-utils/key.h>
#include <fcitx-utils/log.h>
#include <fcitx-utils/keysym.h>
#include <fcitx-utils/textformatflags.h>
#include <fcitx-utils/trackableobject.h>
#include <fcitx/text.h>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cctype>
#include <cerrno>
#include <cstdlib>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace {

// ---------------- protobuf 手写编码 ----------------
void putVarint(std::string &out, uint64_t v) {
    while (v >= 0x80) {
        out.push_back(char((v & 0x7f) | 0x80));
        v >>= 7;
    }
    out.push_back(char(v));
}
void putTag(std::string &out, int field, int wire) { putVarint(out, (uint64_t(field) << 3) | wire); }
void putVarintField(std::string &out, int f, uint64_t v) { putTag(out, f, 0); putVarint(out, v); }
void putBytesField(std::string &out, int f, const std::string &d) {
    putTag(out, f, 2);
    putVarint(out, d.size());
    out += d;
}

// ---------------- 极简 protobuf 读取 ----------------
struct Reader {
    const uint8_t *p;
    size_t n;
    Reader(const uint8_t *p_, size_t n_) : p(p_), n(n_) {}
    bool varint(uint64_t &v) {
        v = 0;
        int shift = 0;
        while (n > 0 && shift < 64) {
            uint8_t b = *p++;
            n--;
            v |= uint64_t(b & 0x7f) << shift;
            if (!(b & 0x80)) return true;
            shift += 7;
        }
        return false;
    }
    bool tag(uint32_t &field, uint32_t &wire) {
        uint64_t v;
        if (!varint(v)) return false;
        field = uint32_t(v >> 3);
        wire = uint32_t(v & 7);
        return true;
    }
    bool bytes(std::string &out) {
        uint64_t len;
        if (!varint(len)) return false;
        if (len > n) return false;
        out.assign(reinterpret_cast<const char *>(p), len);
        p += len;
        n -= len;
        return true;
    }
    bool skip(uint32_t wire) {
        uint64_t v;
        switch (wire) {
        case 0: return varint(v);
        case 1: if (n < 8) return false; p += 8; n -= 8; return true;
        case 2: { std::string s; return bytes(s); }
        case 5: if (n < 4) return false; p += 4; n -= 4; return true;
        default: return false;
        }
    }
};

struct Candidate {
    std::string value;
    std::string id;
    std::string comment;  // 字段 9：只显示服务自己合成的候选（常用语 ⬡ 等）的标记
};

struct EngineReply {
    int type = 0;  // 1=emit 2=candidateUpdate 3=pending 4=config
    std::string emit;
    std::string pending;  // 组合文本（type=3 时）
    std::string raw;      // 组合文本去掉自动加的音节分隔，回车时原样上屏
    std::vector<Candidate> candidates;
    int focused = 0;
    bool hasMore = false;  // 候选后面还有（原生服务的自定义字段 3），翻到末尾时要
};

// ---------------- bridge 客户端（异步读） ----------------
class Bridge {
public:
    ~Bridge() { disconnect(); }
    int fd() const { return fd_; }
    bool native() const { return native_; }
    bool connected() const { return fd_ >= 0; }

    // 同步部分只做连接与握手（一次性，等待极短）；之后由事件循环读
    bool connectAndCreate() {
        disconnect();
        const auto now = std::chrono::steady_clock::now();
        if (hasLastFail_ && now - lastFail_ < std::chrono::milliseconds(500))
            return false;
        native_ = connectUnix();
        if (!native_) {
            lastFail_ = now;
            hasLastFail_ = true;
            return false;
        }
        std::string meta;
        if (!recvFrame(meta, 500) || meta.rfind("PID:", 0) != 0) {
            disconnect();
            lastFail_ = now;
            hasLastFail_ = true;
            return false;
        }
        hasLastFail_ = false;
        pid_ = uint32_t(strtoul(meta.c_str() + 4, nullptr, 10));
        // CREATE_SESSION（响应异步读）
        std::string v1;
        std::string det;
        putVarintField(det, 1, 3);
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, appInfo());
        return send(v1);
    }

    // 原生服务 minime-engined
    bool connectUnix() {
        const char *rt = getenv("XDG_RUNTIME_DIR");
        sockaddr_un a{};
        a.sun_family = AF_UNIX;
        if (rt && *rt)
            snprintf(a.sun_path, sizeof(a.sun_path), "%s/minime/engine.sock", rt);
        else
            snprintf(a.sun_path, sizeof(a.sun_path), "/tmp/minime-%d/engine.sock", int(getuid()));
        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0) return false;
        if (::connect(fd_, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0) {
            disconnect();
            return false;
        }
        int fl = ::fcntl(fd_, F_GETFL, 0);
        if (fl >= 0) ::fcntl(fd_, F_SETFL, fl | O_NONBLOCK);
        return true;
    }

    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    std::string appInfo() const {
        std::string app;
        putVarintField(app, 1, pid_);
        putBytesField(app, 4, "fcitx5-minime");
        putVarintField(app, 5, 1);
        return app;
    }

    // 发送一个 CommandV1（非阻塞写，带有限等待，避免主线程卡死）
    bool send(const std::string &v1) {
        if (fd_ < 0) return false;
        std::string base;
        putVarintField(base, 1, 1);
        putBytesField(base, 2, v1);
        uint8_t hdr[4] = {uint8_t(base.size() & 0xff), uint8_t((base.size() >> 8) & 0xff),
                          uint8_t((base.size() >> 16) & 0xff), uint8_t((base.size() >> 24) & 0xff)};
        if (!writeAll(hdr, 4) || !writeAll(base.data(), base.size())) {
            disconnect();
            return false;
        }
        return true;
    }

    // 读一条已到达的消息（非阻塞）；返回 false = 暂无/断开
    bool pollFrame(std::string &out) {
        if (fd_ < 0) return false;
        pollfd pfd{fd_, POLLIN, 0};
        int r = ::poll(&pfd, 1, 0);
        if (r <= 0) return false;
        return recvFrame(out, 50);
    }

    bool recvFrame(std::string &out, int timeoutMs) {
        if (fd_ < 0) return false;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        // 可读却读不到完整一帧 = 对端关了或半帧超时。必须断开，
        // 否则关掉的 socket 一直可读，fcitx5 的 IO 回调会空转占满 CPU。
        uint8_t hdr[4];
        if (!readAll(hdr, 4, deadline)) {
            disconnect();
            return false;
        }
        uint32_t n = uint32_t(hdr[0]) | (uint32_t(hdr[1]) << 8) | (uint32_t(hdr[2]) << 16) |
                     (uint32_t(hdr[3]) << 24);
        if (n > (16u << 20) || (out.resize(n), !readAll(out.data(), n, deadline))) {
            disconnect();
            return false;
        }
        return true;
    }

private:
    bool writeAll(const void *p, size_t n) {
        const char *c = static_cast<const char *>(p);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
        while (n > 0) {
            ssize_t r = ::send(fd_, c, n, MSG_NOSIGNAL);
            if (r > 0) {
                c += r;
                n -= size_t(r);
                continue;
            }
            if (r < 0 && errno == EINTR) continue;
            if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now()).count());
                if (left <= 0) return false;
                pollfd pfd{fd_, POLLOUT, 0};
                int pr = ::poll(&pfd, 1, left);
                if (pr < 0 && errno == EINTR) continue;
                if (pr <= 0) return false;
                continue;
            }
            return false;
        }
        return true;
    }
    bool readAll(void *p, size_t n, std::chrono::steady_clock::time_point deadline) {
        char *c = static_cast<char *>(p);
        while (n > 0) {
            ssize_t r = ::recv(fd_, c, n, 0);
            if (r > 0) {
                c += r;
                n -= size_t(r);
                continue;
            }
            if (r == 0) return false;
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   deadline - std::chrono::steady_clock::now()).count());
                if (left <= 0) return false;
                pollfd pfd{fd_, POLLIN, 0};
                int pr = ::poll(&pfd, 1, left);
                if (pr < 0 && errno == EINTR) continue;
                if (pr <= 0) return false;
                continue;
            }
            return false;
        }
        return true;
    }

    int fd_ = -1;
    bool native_ = false;
    uint32_t pid_ = 0;
    bool hasLastFail_ = false;
    std::chrono::steady_clock::time_point lastFail_;
};

// ---------------- 响应解析 ----------------
bool parseResp(const std::string &raw, EngineReply &out) {
    std::string body;
    {
        Reader r(reinterpret_cast<const uint8_t *>(raw.data()), raw.size());
        uint32_t f, w;
        while (r.tag(f, w)) {
            if (f == 2 && w == 2) {
                if (!r.bytes(body)) return false;
            } else if (!r.skip(w)) {
                return false;
            }
        }
    }
    Reader r(reinterpret_cast<const uint8_t *>(body.data()), body.size());
    uint32_t f, w;
    while (r.tag(f, w)) {
        if (f == 2 && w == 2) {
            std::string det;
            if (!r.bytes(det)) return false;
            // Detail
            Reader dr(reinterpret_cast<const uint8_t *>(det.data()), det.size());
            uint32_t df, dw;
            EngineReply rep;
            while (dr.tag(df, dw)) {
                if (df == 1 && dw == 0) {
                    uint64_t v;
                    if (!dr.varint(v)) return false;
                    rep.type = int(v);
                } else if (df == 2 && dw == 2) {
                    std::string cl;
                    if (!dr.bytes(cl)) return false;
                    // CandidateList{ repeated CandidateWord candidates=1; focused=2 }
                    Reader cr(reinterpret_cast<const uint8_t *>(cl.data()), cl.size());
                    uint32_t cf, cw;
                    while (cr.tag(cf, cw)) {
                        if (cf == 1 && cw == 2) {
                            std::string cwd;
                            if (!cr.bytes(cwd)) return false;
                            Candidate c;
                            Reader wr(reinterpret_cast<const uint8_t *>(cwd.data()), cwd.size());
                            uint32_t wf, ww;
                            while (wr.tag(wf, ww)) {
                                if (wf == 1 && ww == 2) {
                                    if (!wr.bytes(c.value)) return false;
                                } else if (wf == 2 && ww == 2) {
                                    if (!wr.bytes(c.id)) return false;
                                } else if (wf == 9 && ww == 2) {
                                    if (!wr.bytes(c.comment)) return false;
                                } else if (!wr.skip(ww)) {
                                    return false;
                                }
                            }
                            rep.candidates.push_back(std::move(c));
                        } else if (cf == 2 && cw == 0) {
                            uint64_t v;
                            if (!cr.varint(v)) return false;
                            rep.focused = int(v);
                        } else if (cf == 3 && cw == 0) {
                            uint64_t v;
                            if (!cr.varint(v)) return false;
                            rep.hasMore = v != 0;
                        } else if (!cr.skip(cw)) {
                            return false;
                        }
                    }
                } else if (df == 3 && dw == 2) {
                    std::string pl;
                    if (!dr.bytes(pl)) return false;
                    // PendingList{ repeated PendingWord pengdings=1 }
                    Reader pr(reinterpret_cast<const uint8_t *>(pl.data()), pl.size());
                    uint32_t pf, pw;
                    while (pr.tag(pf, pw)) {
                        if (pf == 1 && pw == 2) {
                            std::string wd;
                            if (!pr.bytes(wd)) return false;
                            Reader wr2(reinterpret_cast<const uint8_t *>(wd.data()), wd.size());
                            uint32_t wf2, ww2;
                            uint64_t segType = 0;
                            std::string v;
                            while (wr2.tag(wf2, ww2)) {
                                if (wf2 == 7 && ww2 == 2) {
                                    if (!wr2.bytes(v)) return false;
                                } else if (wf2 == 1 && ww2 == 0) {
                                    if (!wr2.varint(segType)) return false;
                                } else if (!wr2.skip(ww2)) {
                                    return false;
                                }
                            }
                            // 原生服务：类型 6 是引擎自动切分的音节分隔（显示成 '）
                            v.erase(std::remove(v.begin(), v.end(), '\0'), v.end());
                            rep.pending += v;
                            if (segType != 6) rep.raw += v;
                        } else if (!pr.skip(pw)) {
                            return false;
                        }
                    }
                } else if (df == 4 && dw == 2) {
                    if (!dr.bytes(rep.emit)) return false;
                } else if (!dr.skip(dw)) {
                    return false;
                }
            }
            if (rep.type) out = std::move(rep);
        } else if (!r.skip(w)) {
            return false;
        }
    }
    return true;
}

// ---------------- fcitx5 引擎 ----------------
class MinimeEngine;

// ---------------- 配置（fcitx5 配置工具里自动出设置界面，存 ~/.config/fcitx5/conf/minime.conf） ----------------
// 模糊音：位号和服务、引擎的 SessionConfig 模糊音开关一致（SessionConfig+0x0d+位号，见 docs/input.md）
FCITX_CONFIGURATION(MinimeFuzzyConfig,
    fcitx::Option<bool> zzh{this, "ZZh", "z ⇄ zh", false};
    fcitx::Option<bool> cch{this, "CCh", "c ⇄ ch", false};
    fcitx::Option<bool> ssh{this, "SSh", "s ⇄ sh", false};
    fcitx::Option<bool> nl{this, "NL", "n ⇄ l", false};
    fcitx::Option<bool> rl{this, "RL", "r ⇄ l", false};
    fcitx::Option<bool> hf{this, "HF", "h ⇄ f", false};
    fcitx::Option<bool> gk{this, "GK", "g ⇄ k", false};
    fcitx::Option<bool> anAng{this, "AnAng", "an ⇄ ang", false};
    fcitx::Option<bool> enEng{this, "EnEng", "en ⇄ eng", false};
    fcitx::Option<bool> inIng{this, "InIng", "in ⇄ ing", false};
    fcitx::Option<bool> ianIang{this, "IanIang", "ian ⇄ iang", false};
    fcitx::Option<bool> uanUang{this, "UanUang", "uan ⇄ uang", false};
    fcitx::Option<bool> huiFei{this, "HuiFei", "hui ⇄ fei", false};
    fcitx::Option<bool> huangWang{this, "HuangWang", "huang ⇄ wang", false};);

/* 联网功能由独立的 minime-netd 读同一个配置文件的 [Network] 段（见 netd/src/config.rs），
 * 输入法本体不联网，这里只负责在面板上提供开关。 */
FCITX_CONFIGURATION(MinimeNetworkConfig,
    fcitx::Option<bool> enabled{this, "Enabled", "启用联网功能（需要 minime-netd 服务）", false};
    fcitx::Option<bool> clipboardSync{this, "ClipboardSync", "跨设备剪贴板", true};
    fcitx::Option<bool> imageSync{this, "ImageSync", "跨设备发送图片（经腾讯云存储中转）", false};
    fcitx::Option<bool> cloudInput{this, "CloudInput", "云输入（实验：打字时拼音会发到服务器）", false};
    fcitx::Option<bool> userDictSync{this, "UserDictSync", "同步个人词库（经腾讯云存储中转）", false};
    fcitx::Option<std::string> deviceName{this, "DeviceName", "对端看到的设备名", "Linux"};);

FCITX_CONFIGURATION(MinimeConfig,
    fcitx::Option<bool> traditional{this, "Traditional", "输出繁体字", false};
    fcitx::Option<bool> wholeSentence{this, "WholeSentence", "整句候选（e2e 模型）", true};
    fcitx::Option<bool> shiftToggle{this, "ShiftToggle", "单按 Shift 切换中英文", false};
    fcitx::Option<bool> asciiPunct{this, "AsciiPunct", "使用英文标点", false};
    // 自动加空格（手机设置里的同名开关）：中文和英文之间、中文和数字之间
    fcitx::Option<bool> autoSpaceAbc{this, "AutoSpaceAbc", "中文和英文之间自动加空格", false};
    fcitx::Option<bool> autoSpace123{this, "AutoSpace123", "中文和数字之间自动加空格", false};
    fcitx::KeyListOption punctToggleKey{this, "PunctToggleKey", "中英文标点切换快捷键",
                                        {fcitx::Key("Control+period")}, fcitx::KeyListConstrain()};
    fcitx::KeyListOption voiceKey{this, "VoiceKey", "语音输入（按住说话；或按一下开始、再按一下/回车结束）",
                                  {fcitx::Key("Control+Alt+space")}, fcitx::KeyListConstrain()};
    // 长按单个 Alt 说话：RightAlt / LeftAlt / None（短按和 Alt 组合键照常交给应用）
    fcitx::Option<std::string> voiceHoldKey{this, "VoiceHoldKey", "长按说话（RightAlt / LeftAlt / None）", "RightAlt"};
    // 表情候选：Emoji / Kaomoji（颜文字）/ None
    fcitx::Option<std::string> emojiMode{this, "EmojiMode", "表情候选（Emoji / Kaomoji / None）", "Emoji"};
    fcitx::Option<int, fcitx::IntConstrain> pageSize{this, "PageSize", "每页候选数", 9, fcitx::IntConstrain(3, 9)};
    // 输入方案：Quanpin / Shuangpin / Wubi。双拼方案：Sogou / Microsoft / Xiaohe / PinyinJiaJia / Ziranma / Ziguang / ABC
    // 五笔 86/98/新世纪 由加载哪个 swbd 词库决定（设置程序管 dicts/core_conf.tsv）
    fcitx::Option<std::string> inputScheme{this, "InputScheme", "输入方案（Quanpin / Shuangpin / Wubi）", "Quanpin"};
    fcitx::Option<std::string> shuangpinScheme{this, "ShuangpinScheme",
        "双拼方案（Sogou / Microsoft / Xiaohe / PinyinJiaJia / Ziranma / Ziguang / ABC）", "Xiaohe"};
    fcitx::Option<bool> wubiTopCommit{this, "WubiTopCommit", "五笔：第五码顶字上屏", true};
    fcitx::Option<bool> wubiUniqueCommit{this, "WubiUniqueCommit", "五笔：四码唯一候选自动上屏", false};
    fcitx::Option<bool> wubiPinyin{this, "WubiPinyin", "五笔：拼音混输", false};
    fcitx::Option<MinimeFuzzyConfig> fuzzy{this, "Fuzzy", "模糊音"};
    fcitx::Option<MinimeNetworkConfig> network{this, "Network", "联网功能"};);

static uint32_t fuzzyMask(const MinimeFuzzyConfig &f) {
    uint32_t m = 0;
    auto bit = [&m](bool on, int b) { if (on) m |= 1u << b; };
    bit(*f.nl, 0); bit(*f.rl, 1); bit(*f.hf, 2); bit(*f.gk, 3); bit(*f.anAng, 4); bit(*f.ianIang, 5);
    bit(*f.uanUang, 6); bit(*f.cch, 7); bit(*f.ssh, 8); bit(*f.zzh, 9); bit(*f.huiFei, 10);
    bit(*f.enEng, 11); bit(*f.inIng, 12); bit(*f.huangWang, 14);
    return m;
}

// 可点击候选
class MinimeCandidateWord : public fcitx::CandidateWord {
public:
    MinimeCandidateWord(fcitx::Text text, MinimeEngine *engine, int index, const std::string &comment = {})
        : fcitx::CandidateWord(std::move(text)), engine_(engine), index_(index) {
        if (!comment.empty())
            setComment(fcitx::Text(comment));
    }
    void select(fcitx::InputContext *ic) const override;
    int index() const { return index_; }

private:
    MinimeEngine *engine_;
    int index_;
};

// 候选右键菜单：删除这个词（加进引擎的用户屏蔽表，以后不再出现）
class MinimeCandidateActions : public fcitx::ActionableCandidateList {
public:
    explicit MinimeCandidateActions(MinimeEngine *engine) : engine_(engine) {}
    bool hasAction(const fcitx::CandidateWord &cand) const override;
    std::vector<fcitx::CandidateAction> candidateActions(const fcitx::CandidateWord &) const override {
        fcitx::CandidateAction a;
        a.setId(0);
        a.setText("删除这个词（以后不再出现）");
        return {a};
    }
    void triggerAction(const fcitx::CandidateWord &cand, int id) override;

private:
    MinimeEngine *engine_;
};

// 每个输入框各自的状态（fcitx5 的 InputContextProperty）。组合本身不跨输入框保留：换输入框时丢掉
struct MinimeIcState : public fcitx::InputContextProperty {
    bool english = false;       // 英文模式（单按 Shift 切换）
    bool dq = false, sq = false; // 中文引号配对
    std::string recent;         // 这个输入框里最近上屏的文字（应用不给周围文字时当上下文）
    std::string unsynced;       // 上次应用更新周围文字之后我们上屏/放过的文字（应用更新周围文字是异步的）
    bool surStale = false;      // 回车、方向键等之后应用还没报新的周围文字：手上的是旧的，不能用
};

// 候选列表：鼠标点翻页箭头（或滚轮）时同步光标、要更多候选；已到末尾但服务还有候选时下一页箭头也可用
class MinimeCandidateList : public fcitx::CommonCandidateList {
public:
    explicit MinimeCandidateList(MinimeEngine *engine) : engine_(engine) {}
    bool hasNext() const override;
    void next() override;
    void prev() override;

private:
    MinimeEngine *engine_;
};

// 中文标点：输入法打开时，ASCII 标点换成全角中文标点。引号成对交替。
std::string chinesePunct(uint32_t c, bool &dq, bool &sq) {
    switch (c) {
    case ',': return "，";
    case '.': return "。";
    case '?': return "？";
    case '!': return "！";
    case ';': return "；";
    case ':': return "：";
    case '\\': return "、";
    case '(': return "（";
    case ')': return "）";
    case '[': return "【";
    case ']': return "】";
    case '<': return "《";
    case '>': return "》";
    case '^': return "……";
    case '_': return "——";
    case '$': return "￥";
    case '~': return "～";
    case '"': dq = !dq; return dq ? "“" : "”";
    case '\'': sq = !sq; return sq ? "‘" : "’";
    default: return "";
    }
}

// UTF-8：从字节下标 start 起前 nchars 个字符结束的字节下标
size_t utf8Bytes(const std::string &s, size_t start, size_t nchars) {
    size_t i = start;
    while (i < s.size() && nchars) {
        unsigned char c = static_cast<unsigned char>(s[i]);
        i += c < 0x80 ? 1 : (c & 0xe0) == 0xc0 ? 2 : (c & 0xf0) == 0xe0 ? 3 : (c & 0xf8) == 0xf0 ? 4 : 1;
        nchars--;
    }
    return i < s.size() ? i : s.size();
}
size_t utf8Bytes(const std::string &s, size_t nchars) { return utf8Bytes(s, 0, nchars); }
size_t utf8Count(const std::string &s) {
    size_t n = 0;
    for (unsigned char c : s)
        if ((c & 0xc0) != 0x80) n++;
    return n;
}
// 只留最后 n 个字符
std::string utf8Tail(const std::string &s, size_t n) {
    size_t total = utf8Count(s);
    return total <= n ? s : s.substr(utf8Bytes(s, total - n));
}
// 第一个 / 最后一个 Unicode 字符（UTF-8 解码；空串返回 0）
uint32_t utf8Decode(const std::string &s, size_t i) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 0 : c < 0xe0 ? 1 : c < 0xf0 ? 2 : 3;
    uint32_t cp = n == 0 ? c : n == 1 ? c & 0x1f : n == 2 ? c & 0x0f : c & 0x07;
    for (int k = 1; k <= n && i + k < s.size(); k++) cp = (cp << 6) | ((unsigned char)s[i + k] & 0x3f);
    return cp;
}
uint32_t utf8First(const std::string &s) { return s.empty() ? 0 : utf8Decode(s, 0); }
uint32_t utf8Last(const std::string &s) {
    if (s.empty()) return 0;
    size_t i = s.size() - 1;
    while (i > 0 && ((unsigned char)s[i] & 0xc0) == 0x80) i--;
    return utf8Decode(s, i);
}
bool isHan(uint32_t c) {
    return (c >= 0x3400 && c <= 0x4dbf) || (c >= 0x4e00 && c <= 0x9fff) || (c >= 0xf900 && c <= 0xfaff) ||
           (c >= 0x20000 && c <= 0x2fa1f);
}
bool isAsciiAlpha(uint32_t c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool isAsciiDigit(uint32_t c) { return c >= '0' && c <= '9'; }

bool endsWith(const std::string &s, const std::string &t) {
    return s.size() >= t.size() && s.compare(s.size() - t.size(), t.size(), t) == 0;
}

class MinimeEngine : public fcitx::InputMethodEngineV2 {
public:
    explicit MinimeEngine(fcitx::Instance *instance) : instance_(instance) {
        instance_->inputContextManager().registerProperty("minimeState", &icState_);
        // 应用更新了周围文字：之前上屏的已经反映进去了，不用再在本地补
        surroundingWatch_ = instance_->watchEvent(
            fcitx::EventType::InputContextSurroundingTextUpdated, fcitx::EventWatcherPhase::Default,
            [this](fcitx::Event &e) {
                if (auto *ic = static_cast<fcitx::InputContextEvent &>(e).inputContext()) {
                    auto &state = st(ic);
                    state.unsynced.clear();
                    state.surStale = false;
                    if (debugOn()) {
                        const auto &sur = ic->surroundingText();
                        FCITX_INFO() << "[minime] 周围文字更新 valid=" << sur.isValid() << " cursor=" << sur.cursor()
                                     << " text=[" << utf8Tail(sur.text(), 40) << "]";
                    }
                }
            });
        reloadConfig();
    }

    const fcitx::Configuration *getConfig() const override { return &config_; }
    void setConfig(const fcitx::RawConfig &raw) override {
        config_.load(raw, true);
        fcitx::safeSaveAsIni(config_, kConfFile);
        applyConfig();
    }
    // 改名前（wetype）的配置：新文件还没有时改名过来（engined / netd 也做同样的事，谁先谁做）
    static void migrateLegacyConfig() {
        const char *c = getenv("XDG_CONFIG_HOME"), *h = getenv("HOME");
        std::string base = c && *c ? std::string(c) : std::string(h ? h : "") + "/.config";
        std::string oldc = base + "/fcitx5/conf/wetype.conf", newc = base + "/fcitx5/conf/minime.conf";
        if (access(newc.c_str(), F_OK) != 0 && access(oldc.c_str(), F_OK) == 0)
            rename(oldc.c_str(), newc.c_str());
    }

    void reloadConfig() override {
        migrateLegacyConfig();
        fcitx::readAsIni(config_, kConfFile);
        applyConfig();
    }
    ~MinimeEngine() override {
        holdTimer_.reset();
        voiceClose();
        teardown();
    }

    void activate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) override {
        ic_ = event.inputContext();
        ensureConnected();
        clearState();
    }
    void deactivate(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) override {
        // 切走输入法：丢弃未完成的组合（引擎也要清，否则下次接着旧拼音打）
        shiftAlone_ = false;
        if (voiceActive()) voiceCancel();
        disarmHold(false);
        if (isComposing()) resetEngine();
        clearState();
        auto *ic = event.inputContext();
        if (ic) {
            forgetContext(ic);
            ic->inputPanel().reset();
            ic->updatePreedit();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        ic_ = nullptr;
    }
    // 失去焦点、应用要求重置时调用
    void reset(const fcitx::InputMethodEntry &, fcitx::InputContextEvent &event) override {
        shiftAlone_ = false;
        if (voiceActive() && voiceIc_.get() == event.inputContext()) voiceCancel();   // 失焦：丢掉这次说话
        if (holdArmed_ && holdIc_.get() == event.inputContext()) disarmHold(false);
        if (isComposing()) resetEngine();
        clearState();
        if (auto *ic = event.inputContext())
            forgetContext(ic);      // 失焦、光标被挪走等：刚上屏的文字不再是光标前的内容
        if (ic_ == event.inputContext()) updateUi();
    }

    void keyEvent(const fcitx::InputMethodEntry &, fcitx::KeyEvent &keyEvent) override {
        auto *ic = keyEvent.inputContext();
        const fcitx::Key key = keyEvent.key();
        const auto sym = key.sym();
        const auto states = key.states();

        // 密码框、要求关掉输入法的输入框：按键一律不经过输入法（不进引擎、不学习、不上云，Shift 切换也不处理）
        if (ic->capabilityFlags().testAny(fcitx::CapabilityFlags{fcitx::CapabilityFlag::Password,
                                                                 fcitx::CapabilityFlag::Disable})) {
            if (ic_ == ic && isComposing()) {   // 组合中输入框变成了密码框：丢掉组合
                resetEngine();
                clearState();
                updateUi();
            }
            return;
        }

        // 语音输入：说话中接管键盘（松开语音键/再按一次/回车 = 结束，Esc = 取消，其他键忽略）
        // 已经进入停录识别中（voiceStopping_）时不再拦截普通键，仅保留 Esc 取消，避免抢打字丢键
        if (voiceActive() && (!voiceStopping_ || (!keyEvent.isRelease() && sym == FcitxKey_Escape))) {
            if (keyEvent.isRelease()) {
                if (sym == voiceSym_ && !voiceReleased_) {
                    voiceReleased_ = true;
                    keyEvent.filterAndAccept();
                    // 长按 Alt：松开就结束；快捷键按住超过 0.4 秒再松开 = 按住说话，结束；
                    // 快捷键快速按一下 = 开始，等再按一次或回车
                    if (voiceHold_ || std::chrono::steady_clock::now() - voiceT0_ > std::chrono::milliseconds(400))
                        voiceStop();
                }
                return;
            }
            if (sym == voiceSym_ && voiceHold_) {     // 长按时 Alt 的自动重复
                keyEvent.filterAndAccept();
                return;
            }
            if (key.isModifier()) return;
            keyEvent.filterAndAccept();
            if (sym == FcitxKey_Escape) voiceCancel();
            else if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) voiceStop();
            // 按住时的自动重复也是"按下"：语音键松开过之后再按才算结束
            else if (key.checkKeyList(*config_.voiceKey) && voiceReleased_) voiceStop();
            return;
        }
        // 长按单个 Alt 说话：按下先扣住、计时 0.4 秒；期间按了别的键或松开 = 普通用法，
        // 把扣住的 Alt 补发给应用再照常处理；到点还按着 = 开始说话（应用完全看不到这个 Alt）
        if (holdArmed_) {
            if (sym == holdRaw_.sym() && !keyEvent.isRelease()) {
                keyEvent.filterAndAccept();         // 自动重复
                return;
            }
            disarmHold(true);
        } else if (!keyEvent.isRelease() && sym == holdSym() && sym != FcitxKey_None &&
                   !states.testAny(fcitx::KeyStates{fcitx::KeyState::Ctrl, fcitx::KeyState::Shift,
                                                    fcitx::KeyState::Super})) {
            keyEvent.filterAndAccept();
            armHold(ic, keyEvent.rawKey());
            return;
        }
        if (!keyEvent.isRelease() && key.checkKeyList(*config_.voiceKey)) {
            keyEvent.filterAndAccept();
            voiceStart(ic, sym);
            return;
        }

        // 单按 Shift 切换中英文（配置里打开才有）：按下 Shift、中间没按别的键（且未按其他修饰键）就松开算一次
        if (*config_.shiftToggle) {
            const bool isShift = sym == FcitxKey_Shift_L || sym == FcitxKey_Shift_R;
            const bool anyOtherMod = states.testAny(fcitx::KeyStates{
                fcitx::KeyState::Ctrl, fcitx::KeyState::Alt, fcitx::KeyState::Super,
                fcitx::KeyState::Super2, fcitx::KeyState::Hyper, fcitx::KeyState::Meta});
            if (isShift && !keyEvent.isRelease()) {
                shiftAlone_ = !anyOtherMod;
                return;
            }
            if (isShift && keyEvent.isRelease()) {
                if (shiftAlone_ && !anyOtherMod) toggleEnglish(ic);
                shiftAlone_ = false;
                return;
            }
            if (!keyEvent.isRelease()) shiftAlone_ = false;
        }
        if (keyEvent.isRelease()) return;

        // 单独按下的修饰键（Shift、Ctrl……）不处理
        if (key.isModifier()) return;
        // 中英文标点切换（快捷键可配置，默认 Ctrl+.）
        if (key.checkKeyList(*config_.punctToggleKey)) {
            keyEvent.filterAndAccept();
            togglePunct(ic);
            return;
        }
        const bool ctrl = states.test(fcitx::KeyState::Ctrl);
        const bool otherMod = states.test(fcitx::KeyState::Alt) || states.test(fcitx::KeyState::Super) ||
                              states.test(fcitx::KeyState::Super2) || states.test(fcitx::KeyState::Hyper) ||
                              states.test(fcitx::KeyState::Meta);
        if ((ctrl || otherMod) && !st(ic).english && ic_ == ic && isComposing()) {
            // 组合中的 Ctrl 键：Ctrl+A 全选拼音（接着按退格/Delete 清空），Ctrl+退格删一个音节
            if (ctrl && !otherMod && (sym == FcitxKey_a || sym == FcitxKey_A)) {
                keyEvent.filterAndAccept();
                compSelected_ = true;
                updateUi();
                return;
            }
            // Ctrl+Delete：删掉当前高亮的候选（引擎以后不再出这个词）
            if (ctrl && !otherMod && sym == FcitxKey_Delete && !candidates_.empty()) {
                keyEvent.filterAndAccept();
                blockByIndex(focused_);
                return;
            }
            if (ctrl && !otherMod && sym == FcitxKey_BackSpace) {
                keyEvent.filterAndAccept();
                deleteSyllable();
                return;
            }
            // 其他组合键（Ctrl+V、Ctrl+Z、Alt+Tab……）交给应用前先把拼音原样上屏，免得顺序乱
            commitRaw();
            return;
        }
        // 没在组合：带 Ctrl/Alt/Super 的组合键一律交给应用（Ctrl+V、Ctrl+C、Alt+Tab……）
        if (ctrl || otherMod) {
            forgetContext(ic);          // 粘贴、撤销、跳词……之后光标前是什么不知道了
            return;
        }

        if (st(ic).english) {           // 英文模式：所有键原样交给应用（中文后面第一个字母按开关补空格）
            passThrough(keyEvent, ic);
            return;
        }

        if (ic_ != ic) {
            // 换了输入框：上一个框里没打完的组合丢掉
            if (isComposing()) resetEngine();
            clearState();
            ic_ = ic;
        }
        const bool shift = states.test(fcitx::KeyState::Shift);
        const bool composing = isComposing();
        const uint32_t uni = fcitx::Key::keySymToUnicode(sym);

        if (composing) {
            keyEvent.filterAndAccept();     // 组合中的按键都不漏给应用
            if (compSelected_) {            // Ctrl+A 全选过：退格/Delete 清空，其他键取消全选后照常处理
                compSelected_ = false;
                if (sym == FcitxKey_BackSpace || sym == FcitxKey_Delete) {
                    resetEngine();
                    clearState();
                    updateUi();
                    return;
                }
                updateUi();
            }
            if (sym >= FcitxKey_a && sym <= FcitxKey_z) {
                sendLetter(char(sym));
            } else if (sym >= FcitxKey_A && sym <= FcitxKey_Z) {
                sendLetter(char(sym));      // 大写字母原样给引擎：它认得 iPhone、PDF 这类（woyongiPhone → 我用iPhone）
            } else if (sym == FcitxKey_semicolon && semicolonIsKey()) {
                sendLetter(';');
            } else if (sym == FcitxKey_apostrophe && !shift && scheme() != 2) {
                sendLetter('\'');           // 拼音分词符（xi'an → 西安，fan'gan → 反感）
            } else if (sym >= FcitxKey_1 && sym <= FcitxKey_9) {
                if (int(sym - FcitxKey_1) < pageSize_)
                    selectCandidate(currentPage() * pageSize_ + int(sym - FcitxKey_1));
            } else if (sym == FcitxKey_space) {
                selectCandidate(focused_);
            } else if (sym == FcitxKey_Return || sym == FcitxKey_KP_Enter) {
                commitRaw();                // 回车：原样上屏输入的字母
            } else if (sym == FcitxKey_Escape) {
                resetEngine();
                clearState();
                updateUi();
            } else if (sym == FcitxKey_BackSpace && shift) {
                deleteSyllable();           // Shift+退格：删一个音节
            } else if (sym == FcitxKey_BackSpace) {
                sendKey(0, 12);
            } else if (sym == FcitxKey_Left || sym == FcitxKey_Up) {
                moveFocus(-1);
            } else if (sym == FcitxKey_Right || sym == FcitxKey_Down || sym == FcitxKey_Tab) {
                moveFocus(1);
            } else if (sym == FcitxKey_Page_Up || sym == FcitxKey_minus) {
                turnPage(-1);
            } else if (sym == FcitxKey_Page_Down || sym == FcitxKey_equal) {
                turnPage(1);
            } else {
                // 标点：先上屏当前候选（部分选过词时把剩下的也选完），标点跟在后面
                std::string p = punctFor(ic, uni);
                if (!p.empty()) finishWithPunct(p);
            }
            return;
        }

        // 没有组合时（CapsLock 开启时 Shift+字母 会产生小写的 a..z 且 shift=true，同样进组合）
        if (sym >= FcitxKey_a && sym <= FcitxKey_z) {
            keyEvent.filterAndAccept();
            sendContext(ic);            // 新组合开始：先告诉引擎光标前后是什么
            sendLetter(char(sym));
            return;
        }
        if (sym >= FcitxKey_A && sym <= FcitxKey_Z) {
            keyEvent.filterAndAccept();     // 大写字母开头也交给引擎（PDF、Nihao → Ni好 / Nihao）
            sendContext(ic);
            sendLetter(char(sym));
            return;
        }
        // 数字、空格、回车、退格、方向键等原样交给应用；标点换成中文标点（英文标点模式下也原样交给应用）
        if (isAsciiDigit(uni)) {
            passThrough(keyEvent, ic);  // 汉字后面直接打数字：按开关补空格
            return;
        }
        if (*config_.asciiPunct) {
            passThrough(keyEvent, ic);
            return;
        }
        std::string p = punctFor(ic, uni);
        if (!p.empty()) {
            keyEvent.filterAndAccept();
            commit(ic, p);
            return;
        }
        passThrough(keyEvent, ic);      // 空格、回车、退格、方向键等原样交给应用
    }

    // 不经过引擎、原样交给应用的可打印字符：需要自动加空格时由我们上屏（带空格），否则照常放过；
    // 都记进 recent，让「光标前的文字」在应用不给周围文字时也大致准确
    void passThrough(fcitx::KeyEvent &keyEvent, fcitx::InputContext *ic) {
        const uint32_t u = fcitx::Key::keySymToUnicode(keyEvent.key().sym());
        if (u < 0x20 || u >= 0x7f) {                // 回车、退格、方向键等：不碰，但光标前是什么不知道了
            forgetContext(ic);
            return;
        }
        const std::string ch(1, char(u));
        if (!autoSpace(ic, ch).empty()) {
            keyEvent.filterAndAccept();
            commit(ic, ch);
            return;
        }
        noteTyped(ic, ch);
    }

    // 记下经过我们进了输入框的文字
    void noteTyped(fcitx::InputContext *ic, const std::string &text) {
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive)) return;
        auto &state = st(ic);
        state.recent = utf8Tail(state.recent + text, kRecentChars);
        state.unsynced = utf8Tail(state.unsynced + text, kRecentChars);
    }
    void forgetContext(fcitx::InputContext *ic) {
        auto &state = st(ic);
        state.recent.clear();
        state.unsynced.clear();
        state.surStale = true;
    }

    // 在输入法提示里显示一句话（如「中文标点」）
    void showInfo(fcitx::InputContext *ic, const std::string &msg) { instance_->showCustomInputMethodInformation(ic, msg); }

    // 调试日志：存在 $XDG_RUNTIME_DIR/minime-plugin-debug 时打到 fcitx5 的日志（journalctl --user）
    static bool debugOn() {
        static auto last = std::chrono::steady_clock::time_point::min();
        static bool cached = false;
        const auto now = std::chrono::steady_clock::now();
        if (last == std::chrono::steady_clock::time_point::min() || now - last >= std::chrono::seconds(2)) {
            const char *rt = getenv("XDG_RUNTIME_DIR");
            std::string p = std::string(rt && *rt ? rt : "/tmp") + "/minime-plugin-debug";
            cached = access(p.c_str(), F_OK) == 0;
            last = now;
        }
        return cached;
    }

    // 供 MinimeCandidateWord::select 调用（鼠标点选）
    void selectByIndex(int index) { selectCandidate(index); }

    // 删词（自定义命令 205）：引擎把词加进用户屏蔽表并随即发新的候选列表；
    // 服务合成的候选（常用语、日期时间）服务那边忽略，整句候选只在这次组合里不再出
    bool canBlock(int index) const { return index >= 0 && index < int(candidates_.size()); }
    void blockByIndex(int index) {
        if (!canBlock(index) || !ensureConnected() || !bridge_.native()) return;
        const Candidate cand = candidates_[index];
        std::string cw;
        putBytesField(cw, 1, cand.value);
        putBytesField(cw, 2, cand.id);
        std::string det;
        putVarintField(det, 1, 205);
        putBytesField(det, 5, cw);
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (!bridge_.send(v1)) teardown();
        if (ic_) showInfo(ic_, "已删除「" + cand.value + "」");
    }

    // 供 MinimeCandidateList 调用
    bool hasMoreCandidates() const { return hasMore_; }
    // 鼠标翻页：列表自己已经翻过去了（beyondEnd 时是想翻到还没取的那页），这里同步光标、按需要更多候选
    void onListPaged(int page, bool beyondEnd) {
        if (candidates_.empty()) return;
        if (beyondEnd) {
            requestMore(page);          // 回复到了 handleReply 翻过去、重画
            return;
        }
        focused_ = std::clamp(page * pageSize_, 0, int(candidates_.size()) - 1);
        const int pages = (int(candidates_.size()) + pageSize_ - 1) / pageSize_;
        if (hasMore_ && page >= pages - 2) requestMore(-1);
    }

private:
    static constexpr const char *kConfFile = "conf/minime.conf";
    MinimeConfig config_;
    int pageSize_ = 9;
    bool shiftAlone_ = false;   // Shift 按下后还没按别的键

    bool isComposing() const { return !candidates_.empty() || !pending_.empty(); }
    bool compSelected_ = false;  // 组合中按了 Ctrl+A：拼音全选，接着按退格/Delete 清空

    // 删掉最后一个音节：数预编辑最后一段（引擎用 ' 或空格分音节）里的字母，发同样多个退格
    void deleteSyllable() {
        waitIdle(100);
        auto isSylChar = [this](char ch) {
            return std::isalpha(static_cast<unsigned char>(ch)) || (ch == ';' && semicolonIsKey());
        };
        size_t end = pending_.size();
        while (end > 0 && (pending_[end - 1] == '\'' || pending_[end - 1] == ' ')) end--;
        size_t start = end;
        while (start > 0 && isSylChar(pending_[start - 1])) start--;
        // 末尾紧跟用户主动输入的隔音符 ' 时，连同 ' 一起退格删掉
        size_t tail = pending_.size();
        while (tail > end && pending_[tail - 1] == ' ') tail--;
        int n = (start < end) ? int(tail - start) : 1;   // 末尾不是字母（已选的汉字等）：退一格
        for (int i = 0; i < n; i++) sendKey(0, 12);
    }

    // ---------------- 语音输入（minime-asrd，本地识别） ----------------
    // 协议见 asrd/src/main.rs：start → ready / partial <文字>… ；stop → final <文字>；cancel 或断开 = 丢弃
    bool voiceActive() const { return voiceFd_ >= 0; }

    void voiceStart(fcitx::InputContext *ic, fcitx::KeySym sym) {
        if (ic_ == ic && isComposing()) commitRaw();   // 先把打了一半的拼音原样上屏
        const char *rt = getenv("XDG_RUNTIME_DIR");
        struct sockaddr_un a = {};
        a.sun_family = AF_UNIX;
        snprintf(a.sun_path, sizeof(a.sun_path), "%s/minime-asrd.sock", rt && *rt ? rt : "/tmp");
        int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0 || connect(fd, reinterpret_cast<sockaddr *>(&a), sizeof(a)) < 0 ||
            write(fd, "start\n", 6) != 6) {
            if (fd >= 0) close(fd);
            showInfo(ic, "语音服务未运行：先运行 minime-fetch voice，再 systemctl --user enable --now minime-asrd.socket");
            return;
        }
        fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
        voiceFd_ = fd;
        voiceIc_ = ic->watch();
        voiceSym_ = sym;
        voiceT0_ = std::chrono::steady_clock::now();
        voiceStopping_ = false;
        voiceReleased_ = false;
        voiceHold_ = false;
        voiceBuf_.clear();
        voicePartial_.clear();
        voiceStatus_ = "🎙 正在打开麦克风…";
        voiceIo_ = instance_->eventLoop().addIOEvent(
            fd, fcitx::IOEventFlag::In, [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) {
                voiceReadable();
                return true;
            });
        voiceUi();
    }

    void voiceStop() {
        if (!voiceActive() || voiceStopping_) return;
        voiceStopping_ = true;
        if (write(voiceFd_, "stop\n", 5) != 5) {
            voiceClose();
            return;
        }
        voiceStatus_ = "🎙 识别中…";
        voiceUi();
    }

    void voiceCancel() {
        if (!voiceActive()) return;
        if (write(voiceFd_, "cancel\n", 7) < 0) { /* 服务已断开 */ }
        voiceClose();
    }

    // 关连接、清界面（不上屏）
    void voiceClose() {
        voiceIo_.reset();
        if (voiceFd_ >= 0) close(voiceFd_);
        voiceFd_ = -1;
        auto *ic = voiceIc_.get();
        voiceIc_.unwatch();
        voicePartial_.clear();
        if (ic) {
            ic->inputPanel().reset();
            ic->updatePreedit();
            ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
        }
        if (ic && ic == ic_) updateUi();
    }

    void voiceReadable() {
        char buf[4096];
        for (;;) {
            ssize_t n = read(voiceFd_, buf, sizeof(buf));
            if (n > 0) {
                if (voiceBuf_.size() + size_t(n) > (1u << 20)) {
                    voiceClose();
                    return;
                }
                voiceBuf_.append(buf, size_t(n));
                continue;
            }
            if (n == 0 || (errno != EAGAIN && errno != EINTR)) {
                voiceLines();
                if (voiceActive()) voiceClose();      // 服务断开、没给定稿
                return;
            }
            if (errno == EAGAIN) break;
        }
        voiceLines();
    }

    void voiceLines() {
        size_t nl;
        while (voiceActive() && (nl = voiceBuf_.find('\n')) != std::string::npos) {
            std::string line = voiceBuf_.substr(0, nl);
            voiceBuf_.erase(0, nl + 1);
            const size_t sp = line.find(' ');
            const std::string verb = line.substr(0, sp);
            const std::string arg = sp == std::string::npos ? "" : line.substr(sp + 1);
            if (verb == "ready") {
                voiceStatus_ = "🎙 请说话（松开或回车结束，Esc 取消）";
            } else if (verb == "partial") {
                voicePartial_ = arg;
            } else if (verb == "final") {
                auto *ic = voiceIc_.get();
                voiceClose();
                if (ic && !arg.empty()) commit(ic, arg);
                return;
            } else if (verb == "error") {
                auto *ic = voiceIc_.get();
                voiceClose();
                if (ic) showInfo(ic, arg);
                return;
            }
        }
        if (voiceActive()) voiceUi();
    }

    void voiceUi() {
        auto *ic = voiceIc_.get();
        if (!ic) return;
        auto &panel = ic->inputPanel();
        panel.reset();
        std::string status = voiceStatus_;
        if (!voiceStopping_ && voiceStatus_.rfind("🎙 请说话", 0) == 0) {
            auto sec = int(std::chrono::duration_cast<std::chrono::seconds>(
                std::chrono::steady_clock::now() - voiceT0_).count());
            if (sec < 0) sec = 0;
            char buf[96];
            snprintf(buf, sizeof(buf), "🎙 请说话（%d:%02d / 5:00，松开或回车结束，Esc 取消）", sec / 60, sec % 60);
            status = buf;
        }
        panel.setAuxUp(fcitx::Text(status));
        if (!voicePartial_.empty()) {
            fcitx::Text text;
            text.append(voicePartial_, fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
            text.setCursor(int(voicePartial_.size()));
            if (ic->capabilityFlags().test(fcitx::CapabilityFlag::Preedit))
                panel.setClientPreedit(text);
            else
                panel.setPreedit(text);
        }
        ic->updatePreedit();
        ic->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    // 长按 Alt
    fcitx::KeySym holdSym() const {
        const std::string &v = *config_.voiceHoldKey;
        if (v == "RightAlt") return FcitxKey_Alt_R;
        if (v == "LeftAlt") return FcitxKey_Alt_L;
        return FcitxKey_None;
    }
    void armHold(fcitx::InputContext *ic, const fcitx::Key &raw) {
        holdArmed_ = true;
        holdIc_ = ic->watch();
        holdRaw_ = raw;
        const uint64_t at = fcitx::now(CLOCK_MONOTONIC) + 400000;   // 0.4 秒
        if (!holdTimer_) {
            holdTimer_ = instance_->eventLoop().addTimeEvent(CLOCK_MONOTONIC, at, 0,
                                                             [this](fcitx::EventSourceTime *, uint64_t) {
                                                                 onHoldTimer();
                                                                 return true;
                                                             });
        } else {
            holdTimer_->setTime(at);
        }
        holdTimer_->setOneShot();
    }
    // forward：把扣住的 Alt 按下补发给应用（普通 Alt 用法）
    void disarmHold(bool forward) {
        if (!holdArmed_) return;
        holdArmed_ = false;
        if (holdTimer_) holdTimer_->setEnabled(false);
        if (forward)
            if (auto *ic = holdIc_.get()) ic->forwardKey(holdRaw_, false);
    }
    void onHoldTimer() {
        if (!holdArmed_) return;
        holdArmed_ = false;
        auto *ic = holdIc_.get();
        if (!ic) return;
        voiceStart(ic, holdRaw_.sym());
        if (voiceActive()) voiceHold_ = true;
        else ic->forwardKey(holdRaw_, false);       // 语音服务不在：Alt 照常交给应用
    }
    bool holdArmed_ = false;
    fcitx::TrackableObjectReference<fcitx::InputContext> holdIc_;
    fcitx::Key holdRaw_;
    std::unique_ptr<fcitx::EventSourceTime> holdTimer_;
    bool voiceHold_ = false;                      // 这次说话是长按 Alt 开始的（松开即结束）

    int voiceFd_ = -1;
    std::unique_ptr<fcitx::EventSourceIO> voiceIo_;
    fcitx::TrackableObjectReference<fcitx::InputContext> voiceIc_;
    fcitx::KeySym voiceSym_ = FcitxKey_None;      // 语音键的主键（按住说话时等它松开）
    std::chrono::steady_clock::time_point voiceT0_;
    bool voiceStopping_ = false;
    bool voiceReleased_ = false;                  // 开始之后语音键松开过
    std::string voiceBuf_, voicePartial_, voiceStatus_;

    void clearState() {
        candidates_.clear();
        pending_.clear();
        raw_.clear();
        punctAfter_.clear();
        focused_ = 0;
        inflight_ = 0;
        hasMore_ = false;
        moreInflight_ = false;
        wantPage_ = -1;
        compSelected_ = false;
    }

    MinimeIcState &st(fcitx::InputContext *ic) { return *ic->propertyFor(&icState_); }

    // 标点键对应的输出：中文标点模式下是中文标点（引号成对交替），英文标点模式下是原字符；不是标点键返回空
    std::string punctFor(fcitx::InputContext *ic, uint32_t uni) {
        if (*config_.asciiPunct) {
            bool d = false, s = false;
            return chinesePunct(uni, d, s).empty() ? std::string() : std::string(1, char(uni));
        }
        auto &state = st(ic);
        return chinesePunct(uni, state.dq, state.sq);
    }

    void togglePunct(fcitx::InputContext *ic) {
        config_.asciiPunct.setValue(!*config_.asciiPunct);
        fcitx::safeSaveAsIni(config_, kConfFile);
        showInfo(ic, *config_.asciiPunct ? "英文标点" : "中文标点");
    }

    // 组合中按标点：上屏当前候选；部分选词时引擎接着给剩下拼音的候选，一直选首选直到整句上屏，标点跟在后面
    void finishWithPunct(const std::string &p) {
        punctAfter_ += p;
        waitIdle(100);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(300);
        for (int i = 0; i < 16 && isComposing() && !candidates_.empty(); i++) {
            int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now()).count());
            if (left <= 0) break;
            const uint64_t seq = candSeq_;
            selectCandidate(focused_);
            waitUntil([&] { return inflight_ == 0 && (!isComposing() || candSeq_ != seq); }, left);
            if (isComposing() && candSeq_ == seq) break;    // 没等到新候选：标点等上屏时再出
        }
        if (isComposing() && candidates_.empty()) {
            // 有预编辑却没有候选（如误触字符导致无效组合）：原样上屏并跟上标点，避免吞键或残留 punctAfter_
            std::string pa = std::move(punctAfter_);
            punctAfter_.clear();
            std::string raw = raw_.empty() ? pending_ : raw_;
            resetEngine();
            clearState();
            if (ic_ && !raw.empty()) commit(ic_, raw + pa);
            else if (ic_ && !pa.empty()) commit(ic_, pa);
        }
        updateUi();
    }

    void teardown() {
        io_.reset();
        bridge_.disconnect();
    }

    bool ensureConnected() {
        if (bridge_.connected() && io_) return true;
        teardown();
        inflight_ = 0;
        if (!bridge_.connectAndCreate()) return false;
        io_ = instance_->eventLoop().addIOEvent(
            bridge_.fd(), fcitx::IOEventFlag::In,
            [this](fcitx::EventSourceIO *, int, fcitx::IOEventFlags) { return onReadable(); });
        sendConfig();               // 新连接：把配置告诉服务
        return true;
    }

    // 让引擎丢掉当前组合。原生服务认 ESC；官方 server 不认（会当成字符插进去），只能断开重连
    void resetEngine() {
        discarding_ = true;
        punctAfter_.clear();
        waitIdle(100);
        if (bridge_.connected() && bridge_.native()) {
            sendKey(0, 10);
            waitIdle(100);          // 把 ESC 之后的空预编辑/空候选读掉，免得晚到的帧又把组合带回来
        } else {
            teardown();             // 下次按键时重连 = 新会话
        }
        discarding_ = false;
    }

    // 处理一帧响应；返回 false 表示不是有意义的帧
    void handleReply(EngineReply &rep) {
        if (rep.type == 1) {
            if (!bridge_.native() && inflight_ > 0) inflight_--;   // 原生服务总是在后面附带一帧 RESP_PENDING
            if (!discarding_ && ic_ && !rep.emit.empty()) commit(ic_, rep.emit + punctAfter_);
            punctAfter_.clear();
            candidates_.clear();
            pending_.clear();
            raw_.clear();
            focused_ = 0;
        } else if (rep.type == 2) {
            candSeq_++;
            const bool extended = moreInflight_ && rep.candidates.size() > candidates_.size();
            candidates_ = std::move(rep.candidates);
            hasMore_ = rep.hasMore;
            if (extended) {
                // "要更多候选"的回复：保持当前位置；翻页时已经超出末尾的，现在翻过去
                if (wantPage_ >= 0)
                    focused_ = std::min(wantPage_ * pageSize_, int(candidates_.size()) - 1);
            } else {
                focused_ = (rep.focused >= 0 && rep.focused < int(candidates_.size())) ? rep.focused : 0;
            }
            moreInflight_ = false;
            wantPage_ = -1;
        } else if (rep.type == 3) {
            if (inflight_ > 0) inflight_--;
            if (!discarding_) {
                pending_ = std::move(rep.pending);
                raw_ = std::move(rep.raw);
            }
        }
    }

    bool onReadable() {
        std::string payload;
        while (bridge_.pollFrame(payload)) {
            EngineReply rep;
            if (parseResp(payload, rep)) handleReply(rep);
        }
        if (!bridge_.connected()) {
            // 服务断了：会话没了，组合也作废；下次按键时重连
            io_.reset();
            clearState();
            updateUi();
            return false;
        }
        updateUi();
        return true;
    }

    // 等已发出的按键都有了响应（原生服务几毫秒）。回车、选词、重置前用，避免拿旧状态做决定
    void waitIdle(int timeoutMs) { waitUntil([this] { return inflight_ == 0; }, timeoutMs); }

    // 边读响应边等 done() 成立，最多等 timeoutMs
    template <class F>
    void waitUntil(F done, int timeoutMs) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
        while (!done() && bridge_.connected()) {
            int left = int(std::chrono::duration_cast<std::chrono::milliseconds>(
                               deadline - std::chrono::steady_clock::now()).count());
            if (left <= 0) break;
            std::string payload;
            if (!bridge_.recvFrame(payload, left)) break;
            EngineReply rep;
            if (parseResp(payload, rep)) handleReply(rep);
        }
    }

    // 光标前的文字（应用给周围文字时用它，并补上异步还没反映进来的刚上屏文字；不给时用本框最近上屏的）
    std::string textBefore(fcitx::InputContext *ic, size_t chars) {
        const auto &state = st(ic);
        const auto &sur = ic->surroundingText();
        if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) && sur.isValid() && !state.surStale) {
            const std::string &t = sur.text();
            std::string before = t.substr(0, utf8Bytes(t, sur.cursor()));
            // 有的应用（Kate/KTextEditor）报的周围文字里含着还没上屏的拼音（预编辑），光标在它后面：去掉
            if (ic == ic_ && !pending_.empty() && endsWith(before, pending_))
                before.erase(before.size() - pending_.size());
            // 只补应用还没反映进来的那部分（不能补全部 recent：回车、点鼠标换了位置后它就不是光标前的了）
            if (!state.unsynced.empty() && !endsWith(before, state.unsynced)) before += state.unsynced;
            return utf8Tail(before, chars);
        }
        return utf8Tail(state.recent, chars);
    }

    // 自动加空格：光标前是汉字、要上屏的以英文字母/数字开头（或者反过来）时，前面补一个空格。
    // 两个开关分开：中英文之间（AutoSpaceAbc）、中文和数字之间（AutoSpace123）。标点、空格前后都不加
    std::string autoSpace(fcitx::InputContext *ic, const std::string &text) {
        const bool abc = *config_.autoSpaceAbc, num = *config_.autoSpace123;
        if ((!abc && !num) || text.empty()) return {};
        const std::string before = textBefore(ic, 4);
        const uint32_t p = utf8Last(before), f = utf8First(text);
        auto latin = [&](uint32_t c) { return (abc && isAsciiAlpha(c)) || (num && isAsciiDigit(c)); };
        const bool add = (isHan(p) && latin(f)) || (latin(p) && isHan(f));
        if (debugOn()) {
            const auto &state = st(ic);
            FCITX_INFO() << "[minime] 自动空格 光标前=[" << before << "] 上屏=[" << text << "] 加=" << add
                         << " stale=" << state.surStale << " unsynced=[" << state.unsynced << "] recent=["
                         << state.recent << "] sur=" << ic->surroundingText().isValid();
        }
        return add ? " " : "";
    }

    // 上屏，并记住最近上屏的文字（应用不支持周围文字、或还没来得及更新时当上下文用）
    void commit(fcitx::InputContext *ic, const std::string &text0) {
        const std::string text = autoSpace(ic, text0) + text0;
        ic->commitString(text);
        noteTyped(ic, text);
    }

    // 发光标周围文字（官方协议 STR_SEND_TEXT_AROUND_CURSOR=104：112 光标前，113 光标后，114 原因）
    void sendContext(fcitx::InputContext *ic) {
        if (!ensureConnected()) return;
        std::string before, after;
        // 应用标了敏感（隐私）的输入框：不给上下文，服务也不学习、不发云请求（字段 303）
        const bool priv = ic->capabilityFlags().test(fcitx::CapabilityFlag::Sensitive);
        const auto &sur = ic->surroundingText();
        if (priv) {
        } else if (ic->capabilityFlags().test(fcitx::CapabilityFlag::SurroundingText) && sur.isValid()) {
            const std::string &t = sur.text();
            size_t pos = utf8Bytes(t, sur.cursor());
            after = t.substr(pos, utf8Bytes(t, pos, kAfterChars) - pos);
            before = textBefore(ic, kBeforeChars);   // 补上应用还没反映进来的刚上屏文字
        } else {
            before = textBefore(ic, kBeforeChars);   // 应用不给周围文字：只能用自己上屏过的
        }
        std::string det;
        putVarintField(det, 1, 104);
        putBytesField(det, 112, before);
        putBytesField(det, 113, after);
        putVarintField(det, 114, 0);
        if (bridge_.native()) putVarintField(det, 303, priv ? 1 : 0);   // 自定义；Wine 栈不认，不发
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (!bridge_.send(v1)) teardown();
    }

    // 回车：把输入的字母原样上屏（部分选过词时，已选的汉字也一起）
    // 中英文切换：组合中的字母原样上屏；托盘和提示里显示"中"/"英"
    void toggleEnglish(fcitx::InputContext *ic) {
        auto &state = st(ic);
        if (!state.english && ic_ == ic && isComposing()) commitRaw();
        state.english = !state.english;
        ic->updateUserInterface(fcitx::UserInterfaceComponent::StatusArea);
        instance_->showInputMethodInformation(ic);
    }

    std::string subModeLabelImpl(const fcitx::InputMethodEntry &, fcitx::InputContext &ic) override {
        return *config_.shiftToggle ? (st(&ic).english ? "英" : "中") : "";
    }

    void commitRaw() {
        waitIdle(150);
        std::string text = raw_.empty() ? pending_ : raw_;
        if (ic_ && !text.empty()) commit(ic_, text);
        resetEngine();
        clearState();
        updateUi();
    }

    int currentPage() const {
        if (ic_) {
            if (auto list = ic_->inputPanel().candidateList()) {
                if (auto *pg = list->toPageable()) {
                    if (pg->currentPage() >= 0) return pg->currentPage();
                }
            }
        }
        return focused_ / pageSize_;
    }

    void moveFocus(int d) {
        if (candidates_.empty()) return;
        if (d > 0 && hasMore_ && focused_ + d >= int(candidates_.size()) - pageSize_)
            requestMore(-1);            // 光标快到末尾：提前要下一批
        focused_ = std::clamp(focused_ + d, 0, int(candidates_.size()) - 1);
        updateUi();
    }

    void turnPage(int d) {
        if (candidates_.empty()) return;
        const int pages = (int(candidates_.size()) + pageSize_ - 1) / pageSize_;
        const int target = currentPage() + d;
        if (d > 0 && target >= pages && hasMore_) {
            requestMore(target);        // 已经到末尾：等更多候选到了再翻过去
            return;
        }
        const int page = std::clamp(target, 0, pages - 1);
        focused_ = std::min(page * pageSize_, int(candidates_.size()) - 1);
        if (d > 0 && hasMore_ && page >= pages - 2)
            requestMore(-1);            // 快到末尾：提前要下一批
        updateUi();
    }

    // 向原生服务要后面的候选（自定义命令 201）；page >= 0 时回复到了翻到这一页
    void requestMore(int page) {
        if (page >= 0) wantPage_ = page;
        if (moreInflight_ || !bridge_.connected() || !bridge_.native()) return;
        std::string det;
        putVarintField(det, 1, 201);
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (bridge_.send(v1)) moreInflight_ = true;
        else teardown();
    }

    void updateUi() {
        if (voiceActive()) {
            voiceUi();
            return;
        }
        if (!ic_) return;
        auto &panel = ic_->inputPanel();
        panel.reset();
        const std::string &shown = pending_;
        if (!shown.empty()) {
            // 拼音显示在输入的位置（应用支持内联预编辑时），否则显示在候选框上方
            fcitx::Text text;
            text.append(shown, compSelected_ ? fcitx::TextFormatFlags{fcitx::TextFormatFlag::HighLight}
                                                : fcitx::TextFormatFlags{fcitx::TextFormatFlag::Underline});
            text.setCursor(int(shown.size()));
            if (ic_->capabilityFlags().test(fcitx::CapabilityFlag::Preedit))
                panel.setClientPreedit(text);
            else
                panel.setPreedit(text);
        }
        if (!candidates_.empty()) {
            auto list = std::make_unique<MinimeCandidateList>(this);
            list->setPageSize(pageSize_);
            std::vector<std::string> labels;
            for (int i = 0; i < pageSize_; i++) labels.push_back(std::to_string(i + 1) + " ");   // 序号和候选之间留一个空格
            list->setLabels(labels);
            list->setCursorPositionAfterPaging(fcitx::CursorPositionAfterPaging::ResetToFirst);
            for (int i = 0; i < int(candidates_.size()); i++) {
                // 引擎自己的候选备注不显示；服务合成的候选（id 以 \x01minime- 开头）和云候选（☁）带小标记
                const auto &c = candidates_[i];
                bool ours = c.id.rfind("\x01minime-", 0) == 0 || c.comment == "☁";   // ☁ = 云输入带来的候选
                list->append(std::make_unique<MinimeCandidateWord>(fcitx::Text(c.value), this, i,
                                                                   ours ? c.comment : std::string()));
            }
            list->setGlobalCursorIndex(focused_);
            list->setPage(focused_ / pageSize_);
            list->setActionableImpl(std::make_unique<MinimeCandidateActions>(this));
            panel.setCandidateList(std::move(list));
        }
        ic_->updatePreedit();
        ic_->updateUserInterface(fcitx::UserInterfaceComponent::InputPanel);
    }

    void sendLetter(char c) { sendKey(uint8_t(c), 64 /*TEXT_INPUT*/); }

    int scheme() const {
        const std::string &s = *config_.inputScheme;
        return s == "Shuangpin" ? 1 : s == "Wubi" ? 2 : 0;
    }
    // 引擎的双拼方案编号（SessionConfig +0x20，实测见 docs/input.md「双拼与五笔」）
    int shuangpinId() const {
        static const char *const names[] = {"Sogou", "Microsoft", "Xiaohe", "PinyinJiaJia", "Ziranma", "Ziguang", "ABC"};
        for (int i = 0; i < 7; i++)
            if (*config_.shuangpinScheme == names[i]) return i + 1;
        return 3;
    }
    // 搜狗、微软、紫光双拼用分号当韵母键（ing 等）：组合中按分号交给引擎，不当标点
    bool semicolonIsKey() const {
        int id = shuangpinId();
        return scheme() == 1 && (id == 1 || id == 2 || id == 6);
    }

    // 配置变了：页大小本地用；模糊音、繁体、整句候选发给服务（自定义命令 202，Wine 栈不认，不发）
    void applyConfig() {
        pageSize_ = *config_.pageSize;
        if (!*config_.shiftToggle) {
            instance_->inputContextManager().foreach([this](fcitx::InputContext *ic) {
                st(ic).english = false;
                return true;
            });
        }
        if (bridge_.connected()) sendConfig();
        updateUi();
    }

    void sendConfig() {
        if (!bridge_.connected() || !bridge_.native()) return;
        std::string det;
        putVarintField(det, 1, 202);
        putVarintField(det, 300, fuzzyMask(*config_.fuzzy));
        putVarintField(det, 301, *config_.traditional ? 1 : 0);
        putVarintField(det, 302, *config_.wholeSentence ? 1 : 0);
        const std::string &em = *config_.emojiMode;
        putVarintField(det, 304, em == "Kaomoji" ? 1 : em == "None" ? 2 : 0);   // 0 Emoji / 1 颜文字 / 2 不出
        putVarintField(det, 305, scheme());                                       // 0 全拼 / 1 双拼 / 2 五笔
        putVarintField(det, 306, shuangpinId());
        putVarintField(det, 307, (*config_.wubiTopCommit ? 1 : 0) | (*config_.wubiUniqueCommit ? 2 : 0) |
                                     (*config_.wubiPinyin ? 4 : 0));
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (!bridge_.send(v1)) teardown();
    }

    void sendKey(unsigned keyCode, int special) {
        if (!ensureConnected()) return;
        std::string key;
        putVarintField(key, 1, keyCode);
        putVarintField(key, 2, special);
        std::string det;
        putVarintField(det, 1, 1);  // SEND_KEY
        putBytesField(det, 2, key);
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (bridge_.send(v1)) inflight_++;   // 每个键都会回一帧预编辑
        else teardown();
    }

    void selectCandidate(int index) {
        if (index < 0 || index >= int(candidates_.size())) return;
        const Candidate cand = candidates_[index];
        if (!ensureConnected()) return;
        std::string cw;
        putBytesField(cw, 1, cand.value);
        putBytesField(cw, 2, cand.id);
        std::string det;
        putVarintField(det, 1, 2);  // SELECT_CANDIDATE
        putBytesField(det, 5, cw);
        std::string v1;
        putBytesField(v1, 1, det);
        putBytesField(v1, 2, bridge_.appInfo());
        if (bridge_.send(v1)) inflight_++;   // 回上屏（整词）或新的预编辑（部分选词）
        else teardown();
    }

    fcitx::Instance *instance_;
    Bridge bridge_;
    std::unique_ptr<fcitx::EventSourceIO> io_;
    fcitx::InputContext *ic_ = nullptr;
    std::vector<Candidate> candidates_;
    bool hasMore_ = false;      // 服务那边还有候选没给
    bool moreInflight_ = false; // 已经要了，回复还没到
    int wantPage_ = -1;         // 回复到了翻到这一页
    std::string pending_;
    std::string raw_;
    std::string punctAfter_;   // 组合中按了标点：等选词上屏后跟在后面
    uint64_t candSeq_ = 0;     // 收到的候选列表帧计数
    std::unique_ptr<fcitx::HandlerTableEntry<fcitx::EventHandler>> surroundingWatch_;
    fcitx::FactoryFor<MinimeIcState> icState_{[](fcitx::InputContext &) { return new MinimeIcState; }};
    static constexpr size_t kRecentChars = 32, kBeforeChars = 64, kAfterChars = 32;
    int focused_ = 0;
    int inflight_ = 0;         // 已发出、还没收到预编辑/上屏响应的命令数
    bool discarding_ = false;  // resetEngine 丢弃在途组合期间抑制误上屏
};

void MinimeCandidateWord::select(fcitx::InputContext *) const { engine_->selectByIndex(index_); }

bool MinimeCandidateActions::hasAction(const fcitx::CandidateWord &cand) const {
    auto *w = dynamic_cast<const MinimeCandidateWord *>(&cand);
    return w && engine_->canBlock(w->index());
}
void MinimeCandidateActions::triggerAction(const fcitx::CandidateWord &cand, int id) {
    auto *w = dynamic_cast<const MinimeCandidateWord *>(&cand);
    if (w && id == 0) engine_->blockByIndex(w->index());
}

bool MinimeCandidateList::hasNext() const {
    return CommonCandidateList::hasNext() || engine_->hasMoreCandidates();
}
void MinimeCandidateList::next() {
    if (CommonCandidateList::hasNext()) {
        CommonCandidateList::next();
        engine_->onListPaged(currentPage(), false);
    } else {
        engine_->onListPaged(currentPage() + 1, true);
    }
}
void MinimeCandidateList::prev() {
    CommonCandidateList::prev();
    engine_->onListPaged(currentPage(), false);
}

class MinimeAddonFactory : public fcitx::AddonFactory {
public:
    fcitx::AddonInstance *create(fcitx::AddonManager *manager) override {
        return new MinimeEngine(manager->instance());
    }
};

}  // namespace

FCITX_ADDON_FACTORY_V2(minime, MinimeAddonFactory);
