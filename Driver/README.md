# Patchlane専用ドライバー

macOS 13以降のApple Silicon向けAudioServerPlugIn。2ch／8chを別バンドル・別UIDとして公開する。標準の出力音量・ミュートに対応する。

8chでは4組のステレオ入力を4つのバスへルーティングする。2chではステレオ入力をMainへ送る。どちらも共有リング直結と機器セット経由の出力を利用できる。

```sh
bash scripts/build-driver.sh
bash scripts/test-driver.sh
bash scripts/package-driver.sh
```

`build/Patchlane-Installer.pkg` はアプリ、両ドライバー、補助サービスを同梱する。導入時にCore Audioを再起動する。

カスタムプロパティはCFPropertyListとして転送するCFData。

- `lcfg`: 100バイトのv1ルーティング設定。
- `lshm`: 共有領域・公開状態を示す6個のUInt64。
- `lsta`: 書き込み・読み取り・不足フレーム・クロック状態を示す5個のUInt64。

リングは正確なサンプル時刻で読み取る。ゲイン変更は約5msで平滑化し、サンプル位置の遅延は加えない。これは物理的な入出力遅延がゼロという意味ではない。
