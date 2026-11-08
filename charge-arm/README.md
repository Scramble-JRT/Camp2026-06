# Charge Arm（Raspberry Pi / C++）

USBカメラでスマホ画面の電池残量(%)を読み取り、Bluetoothスピーカーで知らせます。アームは人が操作します。

| 状況 | 音声 | アームへの指示（画面・ログ） |
|---|---|---|
| 充電中に残量が `--full`（既定100）%以上 | 「充電完了」 | `REMOVE_CHARGER`（充電器を抜く／スマホを除ける） |
| 充電していない時に残量が `--low`（既定20）%以下 | 「電池残量低下」 | `INSERT_CHARGER`（充電器を差す） |

## 準備
```sh
sudo apt install -y build-essential cmake pkg-config libopencv-dev \
  libtesseract-dev libleptonica-dev tesseract-ocr-eng \
  open-jtalk open-jtalk-mecab-naist-jdic hts-voice-nitech-jp-atr503-m001 \
  pulseaudio-utils alsa-utils
cd charge-arm && cmake -B build && cmake --build build -j4
```

### Bluetoothスピーカー
先に一度ペアリングし、**既定の出力先**にしておきます（アプリは既定の出力先へ再生します）。
```sh
bluetoothctl            # scan on → pair <MAC> → trust <MAC> → connect <MAC>
pactl list short sinks  # bluez_output... が出ること
pactl set-default-sink <bluez_output...の名前>
```

## 実行
```sh
# 1) 読み取り範囲の指定（モニタ接続時。電池の%数字を囲んでEnter。roi.txt に保存）
./build/charge_arm --select-roi --show
# 2) 通常運転（ヘッドレスOK）
./build/charge_arm
```
主なオプション: `--device /dev/video0` `--roi x,y,w,h` `--full 100` `--low 20` `--interval 2` `--state charging|idle` `--debug` `--no-voice`（`--help`相当は不明な引数で表示）

- カメラなし・スピーカーだけの確認: `./build/charge_arm --sim` を実行し、`100` や `15` を入力
- 記録は `charge_log.csv`（時刻, 種別, 内容）に追記されます
- 読めないとき: `--debug` で `debug_crop.png` を確認し、カメラを近づける／範囲を絞る／画面を明るくする。スマホ側で「バッテリー残量(%)を表示」をオンに
- アーム連携を後で足す場合は `main.cpp` の `Controller::sendCommand` に処理を追加
