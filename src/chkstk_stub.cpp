// Stub for ___chkstk_ms that forwards to __chkstk
// This is needed when linking MinGW-built libraries with MSVC linker

extern "C" {
    void __chkstk(void);

    void ___chkstk_ms(void) {
        __chkstk();
    }
}
