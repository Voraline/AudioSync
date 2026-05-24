-keep class com.audiosync.app.MainActivity {
    public native java.lang.String NativeDecodeMp3(byte[]);
    public native void NativeConnect(java.lang.String);
    public native void NativeStartReceiveLoop();
    public native void NativeDisconnect();
    void onCreate(android.os.Bundle);
    void onDestroy();
    void onActivityResult(int, int, android.content.Intent);
}
