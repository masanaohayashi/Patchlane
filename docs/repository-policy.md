# リポジトリの公開範囲

## 含めるもの

| 対象 | 用途 |
| --- | --- |
| `Package.swift`、`Sources/` | アプリと音声エンジンを再ビルドするためのソース |
| `Driver/`、`Shared/`、`Broker/` | 専用ドライバーと共有音声処理のソース |
| `Resources/`、`Uninstaller/` | アプリ設定、entitlements、アンインストーラーのソース |
| `Tests/`、`Tools/` | 回帰テスト、再現可能な診断・計測ツール |
| `scripts/` | ビルド、署名、公証、パッケージ作成、公開対象の監査 |
| `README.md`、`docs/`、`.gitignore` | 現行製品の説明と開発手順 |

## 含めないもの

| 対象 | 扱い |
| --- | --- |
| `.build/`、`build/` | キャッシュ・実行ファイル・公証ログ。配布ZIPはGitHub Releasesへの添付対象 |
| `.local/` | 個人設定、公開準備前のバックアップ、過去の測定・調査記録 |
| `.swiftpm/`、IDEユーザー設定、OSメタデータ | マシン固有の状態 |
| 証明書の秘密鍵、資格情報、プロビジョニングファイル | Gitの対象外。署名資格情報はKeychainで管理 |

`.gitignore` はルートの公開対象を許可リストにしている。新しいトップレベル項目を追加する場合は公開範囲も見直す。

旧開発版の設定移行・旧製品の削除互換処理は公開版から外した。現行Patchlaneの設定読み込み・更新・アンインストールは維持する。過去の計測記録は改名・改変せずローカルに保管する。

監査は `python3 scripts/audit-repository.py --forbid '禁止語'` で実行できる。Gitの無視ルールに従ったファイル一覧を `.build/repository-files.txt` に出力し、ファイル名と本文を大文字小文字を区別せず確認する。

ライセンスはMIT。`LICENSE` に全文を収録する。README用のスクリーンショット `docs/images/patchlane.png` を公開対象に含め、監査ではこのPNGのみをバイナリ画像として許可する。
