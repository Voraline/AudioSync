-keep class com.audiosync.app.MainActivity {
    public native java.lang.String NativeDecodeMp3(byte[]);
    public native void NativeSetConsoleSink();
    public native void NativeClearConsoleSink();
    public native void NativeSetOutputTrimUs(int);
    public native void NativeConnect(java.lang.String);
    public native void NativeStartReceiveLoop();
    public native void NativeDisconnect();
    # Called from C via GetMethodID/CallVoidMethod - must not be removed or renamed by R8
    private void NativeConsoleLog(java.lang.String);
    void onCreate(android.os.Bundle);
    void onDestroy();
    void onActivityResult(int, int, android.content.Intent);
}