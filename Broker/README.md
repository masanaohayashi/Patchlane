# 専用ドライバーの補助サービス

HALが公開する読み取り専用Mach memory-entryを管理し、インストール済みPatchlaneへ渡す。2ch／8chの公開領域は独立している。音声処理や音声コピーは行わない。

カーネル由来のaudit tokenで署名を照合する。公開側はCore Audioのドライバーホスト、取得側は `/Applications/Patchlane.app` に限定する。応答元のroot UIDと署名、メッセージ構造、共有領域のサイズとABIも検証する。

ドライバーにはビルド時に署名済み補助サービスのdesignated requirementを埋め込む。このため、補助サービスの署名はHALのビルドより先に行う。

`scripts/test-broker.sh` は一時的なlaunchdサービスをGUIドメインに登録する実機テスト。通常のドライバーテストとは別に、専用出力を使用していない環境で実行する。
