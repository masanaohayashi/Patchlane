# リリース作成

Developer ID Application／Installer証明書をKeychainに用意し、`notarytool` の資格情報もKeychainプロファイルに保存する。秘密鍵やパスワードはリポジトリに保存しない。

以下の環境変数を設定する。ローカル専用の `.local/release.env` に `export` 形式で記述してもよい。このファイルはGitの対象外。

```sh
export PATCHLANE_SIGN_IDENTITY='Developer ID Application: YOUR NAME (TEAMID)'
export PATCHLANE_INSTALLER_IDENTITY='Developer ID Installer: YOUR NAME (TEAMID)'
export PATCHLANE_NOTARY_PROFILE='YOUR_KEYCHAIN_PROFILE'
bash scripts/package-release.sh
```

スクリプトはReleaseビルド、テスト、Hardened Runtime署名、アプリ＋専用ドライバー入りPKG、アンインストーラー入りZIP、公証、チケット添付、Gatekeeper検証を行う。ローカル環境へのインストールやGitHubへのアップロードは行わない。

公証がAcceptedになった後、PKGとアンインストーラーにチケットを添付してZIPを作り直す。配布物は `build/release/Patchlane-<version>.zip`。結果・提出ID・詳細ログは同じ場所の `notary-<version>/` に残す。

ソース変更後は配布物を再生成する。以前の公証済みZIPが新しいソースを含むとは限らない。
