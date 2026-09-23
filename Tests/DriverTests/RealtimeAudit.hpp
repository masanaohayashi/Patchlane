#pragma once
// Test-only dyld interposer. Scope only the callback, not fixture setup/teardown.
extern "C" void patchlane_audit_enter();
extern "C" void patchlane_audit_leave();
struct RealtimeAuditScope {
    RealtimeAuditScope() { patchlane_audit_enter(); }
    ~RealtimeAuditScope() { patchlane_audit_leave(); }
};
