namespace mm::platform {
bool bleControlAvailable() MM_NONBLOCKING { return false; }
bool bleControlStart(BleControlHandle&, const char*, const char*) { return false; }
void bleControlStop(BleControlHandle& handle) { handle.impl = nullptr; }
size_t bleControlReceive(BleControlHandle&, char*, size_t) MM_NONBLOCKING { return 0; }
bool bleControlPublish(BleControlHandle&, const char*, uint32_t) MM_NONBLOCKING { return false; }
bool bleControlConnected(const BleControlHandle&) MM_NONBLOCKING { return false; }
}  // namespace mm::platform
