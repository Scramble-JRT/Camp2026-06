// Charge Arm (Raspberry Pi / C++ 版)
// USBカメラでスマホ画面の電池残量(%)を読み取り、
//   ・満充電     -> 「充電完了」を音声出力（アームへ「抜く」指示）
//   ・残量低下   -> 「電池残量低下」を音声出力（アームへ「差す」指示）
// アームは人が操作するため、アーム制御は行わない。
#include <opencv2/opencv.hpp>
#include <tesseract/baseapi.h>

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>

using Clock = std::chrono::steady_clock;

// ---------------- 設定 ----------------
struct Config {
    std::string device = "0";            // "0" または "/dev/video0"
    int fullTh = 100;                    // この値以上で「充電完了」
    int lowTh = 20;                      // この値以下で「電池残量低下」
    double intervalSec = 2.0;            // 読み取り間隔
    bool charging = true;                // 開始時、スマホが充電台に載っているか
    bool selectRoi = false;              // 起動時に読み取り範囲をマウスで選ぶ
    bool show = false;                   // プレビュー表示（モニタ接続時のみ）
    bool sim = false;                    // カメラなし。標準入力の数値を読み取り結果とみなす
    bool voice = true;
    bool debug = false;                  // 前処理後の画像を debug_crop.png に保存
    std::string roiFile = "roi.txt";
    std::string logFile = "charge_log.csv";
    std::string wavDir = "wav";
    std::string dic = "/var/lib/mecab/dic/open-jtalk/naist-jdic";
    std::string voiceFile = "/usr/share/hts-voice/nitech-jp-atr503-m001/nitech_jp_atr503_m001.htsvoice";
    cv::Rect roi;                        // 空なら画像全体
};

static std::atomic<bool> g_run{true};
static void onSignal(int) { g_run = false; }

static std::string nowStr() {
    std::time_t t = std::time(nullptr);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", std::localtime(&t));
    return buf;
}

static void usage() {
    std::cout <<
R"(使い方: charge_arm [オプション]
  --device N|/dev/videoN   USBカメラ (既定 0)
  --select-roi             起動時に読み取り範囲を選択して roi.txt に保存 (要モニタ)
  --roi x,y,w,h            読み取り範囲をピクセルで指定
  --full N                 充電完了とみなす残量% (既定 100)
  --low N                  電池残量低下とみなす残量% (既定 20)
  --interval SEC           読み取り間隔 (既定 2)
  --state charging|idle    開始時の状態 (既定 charging)
  --show                   プレビューウィンドウを表示
  --sim                    カメラなしテスト。標準入力に数値(0-100)を入力
  --no-voice               音声出力を無効化
  --debug                  OCR前処理後の画像を debug_crop.png に保存
  --dic PATH --voice PATH  Open JTalk の辞書 / 音声(.htsvoice)
)";
}

static bool parseArgs(int argc, char** argv, Config& c) {
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&]() -> std::string { return i + 1 < argc ? argv[++i] : ""; };
        if (a == "--device") c.device = next();
        else if (a == "--select-roi") c.selectRoi = true;
        else if (a == "--roi") {
            int x, y, w, h;
            if (std::sscanf(next().c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4) return false;
            c.roi = cv::Rect(x, y, w, h);
        }
        else if (a == "--full") c.fullTh = std::atoi(next().c_str());
        else if (a == "--low") c.lowTh = std::atoi(next().c_str());
        else if (a == "--interval") c.intervalSec = std::atof(next().c_str());
        else if (a == "--state") c.charging = (next() != "idle");
        else if (a == "--show") c.show = true;
        else if (a == "--sim") c.sim = true;
        else if (a == "--no-voice") c.voice = false;
        else if (a == "--debug") c.debug = true;
        else if (a == "--dic") c.dic = next();
        else if (a == "--voice") c.voiceFile = next();
        else return false;
    }
    return true;
}

// ---------------- 音声出力 ----------------
// Open JTalk で wav を事前生成し、paplay/aplay で再生する。
// Bluetoothスピーカーを OS の既定の出力先にしておけば、そこから鳴る。
class Speaker {
public:
    explicit Speaker(const Config& c) : cfg_(c) {}

    void prepare() {
        if (!cfg_.voice) return;
        std::system(("mkdir -p '" + cfg_.wavDir + "'").c_str());
        make("done", "充電完了");
        make("low", "電池残量低下");
    }
    void say(const std::string& key) {
        if (!cfg_.voice) return;
        std::string f = cfg_.wavDir + "/" + key + ".wav";
        std::thread([f] {
            std::string cmd = "(paplay '" + f + "' || aplay -q '" + f + "') >/dev/null 2>&1";
            std::system(cmd.c_str());
        }).detach();
    }
private:
    void make(const std::string& key, const std::string& text) {
        std::string wav = cfg_.wavDir + "/" + key + ".wav";
        std::string cmd = "open_jtalk -x '" + cfg_.dic + "' -m '" + cfg_.voiceFile +
                          "' -ow '" + wav + "' >/dev/null 2>&1";
        FILE* p = popen(cmd.c_str(), "w");
        if (p) { std::fputs((text + "\n").c_str(), p); }
        int rc = p ? pclose(p) : -1;
        if (rc != 0) std::cerr << "[警告] 音声ファイルを作れません(" << text
                               << "): open_jtalk と辞書/音声のパスを確認してください\n";
    }
    const Config& cfg_;
};

// ---------------- OCR ----------------
class Ocr {
public:
    bool init() {
        api_ = std::make_unique<tesseract::TessBaseAPI>();
        if (api_->Init(nullptr, "eng") != 0) return false;
        api_->SetVariable("tessedit_char_whitelist", "0123456789%");
        api_->SetPageSegMode(tesseract::PSM_SINGLE_LINE);
        return true;
    }
    // 読み取れたら 0-100、失敗なら -1
    int read(const cv::Mat& frame, cv::Rect roi, bool debug) {
        roi &= cv::Rect(0, 0, frame.cols, frame.rows);
        if (roi.area() <= 0) roi = cv::Rect(0, 0, frame.cols, frame.rows);
        for (bool inv : {false, true}) {
            cv::Mat img = prep(frame(roi), inv);
            if (debug) cv::imwrite(inv ? "debug_crop_inv.png" : "debug_crop.png", img);
            api_->SetImage(img.data, img.cols, img.rows, 1, (int)img.step);
            char* t = api_->GetUTF8Text();
            int v = parse(t ? t : "");
            delete[] t;
            if (v >= 0) return v;
        }
        return -1;
    }
private:
    static cv::Mat prep(const cv::Mat& bgr, bool invert) {
        cv::Mat g, b;
        cv::cvtColor(bgr, g, cv::COLOR_BGR2GRAY);
        double s = std::max(1.0, 90.0 / g.rows);                 // 文字高さを確保
        cv::resize(g, g, cv::Size(), s, s, cv::INTER_CUBIC);
        cv::GaussianBlur(g, g, cv::Size(3, 3), 0);
        cv::threshold(g, b, 0, 255, cv::THRESH_BINARY | cv::THRESH_OTSU);
        if (cv::mean(b)[0] < 128) cv::bitwise_not(b, b);         // 背景を白・文字を黒に揃える
        if (invert) cv::bitwise_not(b, b);
        cv::copyMakeBorder(b, b, 20, 20, 20, 20, cv::BORDER_CONSTANT, cv::Scalar(255));
        return b;
    }
    static int parse(const std::string& t) {
        std::string digits;
        for (char ch : t) if ((ch >= '0' && ch <= '9')) digits += ch; else if (!digits.empty()) break;
        if (digits.empty() || digits.size() > 3) return -1;
        int v = std::atoi(digits.c_str());
        return v <= 100 ? v : -1;
    }
    std::unique_ptr<tesseract::TessBaseAPI> api_;
};

// ---------------- 判定・記録 ----------------
class Controller {
public:
    Controller(const Config& c, Speaker& s) : cfg_(c), spk_(s), charging_(c.charging) {}

    // 確定した読み取り値を処理する
    void onReading(int v) {
        if (v != last_) log("READ", "battery=" + std::to_string(v) + "%");
        last_ = v;
        if (charging_ && v >= cfg_.fullTh) {
            sendCommand("REMOVE_CHARGER", "充電完了 -> スマホを充電台から除ける(充電器を抜く)", v);
            spk_.say("done");
            charging_ = false;
        } else if (!charging_ && v <= cfg_.lowTh) {
            sendCommand("INSERT_CHARGER", "電池残量低下 -> 充電器を差す", v);
            spk_.say("low");
            charging_ = true;
        }
    }
private:
    // アームへの送信口。今回は人間が操作するので表示・記録のみ。
    void sendCommand(const std::string& code, const std::string& text, int v) {
        std::cout << "\n========== [COMMAND] " << code << " : " << text
                  << " (残量 " << v << "%) ==========\n" << std::endl;
        log("COMMAND", code + " battery=" + std::to_string(v) + "%");
    }
    void log(const std::string& kind, const std::string& msg) {
        std::string line = nowStr() + "," + kind + "," + msg;
        std::cout << line << std::endl;
        std::ofstream(cfg_.logFile, std::ios::app) << line << "\n";
    }
    const Config& cfg_;
    Speaker& spk_;
    bool charging_;
    int last_ = -1;
};

// ---------------- main ----------------
int main(int argc, char** argv) {
    Config cfg;
    if (!parseArgs(argc, argv, cfg)) { usage(); return 1; }
    std::signal(SIGINT, onSignal);
    std::signal(SIGTERM, onSignal);

    Speaker spk(cfg);
    spk.prepare();
    Controller ctl(cfg, spk);

    // ---- カメラなしテストモード ----
    if (cfg.sim) {
        std::cout << "テストモード: 電池残量(0-100)を入力してください。Ctrl+Dで終了\n";
        int v;
        while (g_run && std::cin >> v) {
            if (v >= 0 && v <= 100) ctl.onReading(v);
        }
        return 0;
    }

    // ---- 読み取り範囲の読込 ----
    if (cfg.roi.area() == 0) {
        std::ifstream f(cfg.roiFile);
        int x, y, w, h;
        if (f >> x >> y >> w >> h) cfg.roi = cv::Rect(x, y, w, h);
    }

    // ---- USBカメラ (V4L2) ----
    cv::VideoCapture cap;
    bool isNum = !cfg.device.empty() && cfg.device.find_first_not_of("0123456789") == std::string::npos;
    if (isNum) cap.open(std::atoi(cfg.device.c_str()), cv::CAP_V4L2);
    else cap.open(cfg.device, cv::CAP_V4L2);
    if (!cap.isOpened()) { std::cerr << "カメラを開けません: " << cfg.device << "\n"; return 1; }
    cap.set(cv::CAP_PROP_FOURCC, cv::VideoWriter::fourcc('M', 'J', 'P', 'G'));
    cap.set(cv::CAP_PROP_FRAME_WIDTH, 1280);
    cap.set(cv::CAP_PROP_FRAME_HEIGHT, 720);
    cap.set(cv::CAP_PROP_BUFFERSIZE, 1);

    cv::Mat frame;
    for (int i = 0; i < 10; ++i) cap.read(frame);   // 露出が落ち着くまで読み捨て
    if (frame.empty()) { std::cerr << "映像を取得できません\n"; return 1; }

    if (cfg.selectRoi) {
        cfg.roi = cv::selectROI("電池の%数字を囲んでEnter", frame, false);
        cv::destroyAllWindows();
        if (cfg.roi.area() > 0)
            std::ofstream(cfg.roiFile) << cfg.roi.x << " " << cfg.roi.y << " " << cfg.roi.width << " " << cfg.roi.height << "\n";
    }

    Ocr ocr;
    if (!ocr.init()) { std::cerr << "Tesseract を初期化できません\n"; return 1; }

    std::cout << "監視開始 (充電完了>=" << cfg.fullTh << "% / 残量低下<=" << cfg.lowTh
              << "%) 状態=" << (cfg.charging ? "充電中" : "非充電") << "  Ctrl+Cで終了\n";
    if (cfg.roi.area() == 0) std::cout << "[注意] 読み取り範囲が未指定です。--select-roi で指定してください\n";

    auto last = Clock::now() - std::chrono::seconds(1000);
    int pending = -1;
    while (g_run) {
        if (!cap.read(frame) || frame.empty()) {
            std::cerr << "フレーム取得失敗\n";
            std::this_thread::sleep_for(std::chrono::milliseconds(500));
            continue;
        }
        if (cfg.show) {
            cv::Mat view = frame.clone();
            if (cfg.roi.area() > 0) cv::rectangle(view, cfg.roi, cv::Scalar(0, 0, 255), 2);
            cv::imshow("Charge Arm", view);
            if (cv::waitKey(1) == 27) break;
        }
        double el = std::chrono::duration<double>(Clock::now() - last).count();
        if (el < cfg.intervalSec) { if (!cfg.show) std::this_thread::sleep_for(std::chrono::milliseconds(30)); continue; }
        last = Clock::now();

        int v = ocr.read(frame, cfg.roi, cfg.debug);
        if (v < 0) { std::cout << nowStr() << " 数字を読み取れません\n"; pending = -1; continue; }
        if (v == pending) ctl.onReading(v);   // 2回連続で同じ値のときだけ採用（誤認識対策）
        pending = v;
    }
    std::cout << "終了します\n";
    return 0;
}
