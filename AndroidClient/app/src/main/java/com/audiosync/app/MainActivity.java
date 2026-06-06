package com.audiosync.app;
import android.app.Activity;
import android.content.Intent;
import android.net.Uri;
import android.net.wifi.WifiManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.os.PowerManager;
import android.media.AudioManager;
import android.text.InputType;
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.*;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
public class MainActivity extends Activity {
    static { System.loadLibrary("audiosync"); }
    public native String NativeDecodeMp3(byte[] Mp3Data);
    public native void NativeSetConsoleSink();
    public native void NativeClearConsoleSink();
    public native void NativeSetOutputTrimUs(int TrimUs);
    public native void NativeConnect(String Ip);
    public native void NativeStartReceiveLoop();
    public native void NativeDisconnect();
    private static final int PickRequest = 1;
    private static final int MaxConsoleChars = 12000;
    private Handler UiHandler;
    private TextView StatusView, ConsoleView;
    private Button PickBtn, ConnectBtn, DisconnectBtn;
    private EditText IpField, TrimUsField;
    private boolean AudioLoaded = false;
    private WifiManager.WifiLock WifiLock;
    private WifiManager.MulticastLock MulticastLock;
    private PowerManager.WakeLock WakeLock;
    private ScrollView ConsoleScroll;
    private StringBuilder ConsoleText = new StringBuilder();
    @Override
    protected void onCreate(Bundle S) {
        super.onCreate(S);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        UiHandler = new Handler(Looper.getMainLooper());
        NativeSetConsoleSink();
        WifiManager Wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        WifiLock = Wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "AudioSync:Wifi");
        MulticastLock = Wm.createMulticastLock("AudioSync:Multicast");
        MulticastLock.setReferenceCounted(false);
        PowerManager Pm = (PowerManager) getSystemService(POWER_SERVICE);
        WakeLock = Pm.newWakeLock(PowerManager.PARTIAL_WAKE_LOCK, "AudioSync:Wake");
        LinearLayout Root = new LinearLayout(this);
        Root.setOrientation(LinearLayout.VERTICAL);
        Root.setGravity(Gravity.CENTER);
        Root.setPadding(56, 56, 56, 56);
        TextView Title = new TextView(this);
        Title.setText("AudioSync"); Title.setTextSize(30f);
        Title.setGravity(Gravity.CENTER); Title.setPadding(0, 0, 0, 40);
        LinearLayout.LayoutParams FullW = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        LinearLayout.LayoutParams WithMargin = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT);
        WithMargin.setMargins(0, 16, 0, 0);
        PickBtn = new Button(this); PickBtn.setText("PICK MP3 FILE"); PickBtn.setLayoutParams(FullW);
        IpField = new EditText(this); IpField.setHint("PC IP  (e.g. 192.168.1.100)");
        IpField.setSingleLine(true); IpField.setLayoutParams(WithMargin);
        TrimUsField = new EditText(this); TrimUsField.setHint("TRIM US (+ LATER)");
        TrimUsField.setSingleLine(true); TrimUsField.setText("0");
        TrimUsField.setInputType(InputType.TYPE_CLASS_NUMBER | InputType.TYPE_NUMBER_FLAG_SIGNED);
        TrimUsField.setLayoutParams(WithMargin);
        ConnectBtn = new Button(this); ConnectBtn.setText("CONNECT & ARM");
        ConnectBtn.setLayoutParams(WithMargin); ConnectBtn.setEnabled(false);
        DisconnectBtn = new Button(this); DisconnectBtn.setText("DISCONNECT");
        DisconnectBtn.setLayoutParams(WithMargin); DisconnectBtn.setEnabled(false);
        StatusView = new TextView(this); StatusView.setText("Pick an MP3 file to begin.");
        StatusView.setGravity(Gravity.CENTER); StatusView.setPadding(0, 32, 0, 0);
        ConsoleView = new TextView(this); ConsoleView.setText("Console:");
        ConsoleView.setTextSize(12f); ConsoleView.setPadding(16, 16, 16, 16);
        ConsoleView.setTypeface(android.graphics.Typeface.MONOSPACE);
        ConsoleScroll = new ScrollView(this);
        LinearLayout.LayoutParams ConsoleParams = new LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 320);
        ConsoleParams.setMargins(0, 24, 0, 0);
        ConsoleScroll.setLayoutParams(ConsoleParams);
        ConsoleScroll.addView(ConsoleView, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        Root.addView(Title); Root.addView(PickBtn); Root.addView(IpField); Root.addView(TrimUsField);
        Root.addView(ConnectBtn); Root.addView(DisconnectBtn); Root.addView(StatusView); Root.addView(ConsoleScroll);
        ScrollView PageScroll = new ScrollView(this);
        PageScroll.addView(Root, new FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT));
        setContentView(PageScroll);
        PickBtn.setOnClickListener(V -> {
            Intent I = new Intent(Intent.ACTION_GET_CONTENT); I.setType("audio/mpeg");
            startActivityForResult(I, PickRequest);
        });
        ConnectBtn.setOnClickListener(V -> {
            String Ip = IpField.getText().toString().trim();
            if (Ip.isEmpty()) { SetStatus("Enter the PC IP address."); return; }
            Integer TrimUs = ReadTrimUs();
            if (TrimUs == null) { SetStatus("Trim must be a number."); return; }
            StartEngine(Ip, TrimUs);
        });
        DisconnectBtn.setOnClickListener(V -> StopEngine());
    }
    @Override
    protected void onActivityResult(int ReqCode, int ResCode, Intent Data) {
        super.onActivityResult(ReqCode, ResCode, Data);
        if (ReqCode != PickRequest || ResCode != RESULT_OK || Data == null) return;
        Uri FileUri = Data.getData();
        if (FileUri == null) return;
        SetStatus("Reading file...");
        PickBtn.setEnabled(false); ConnectBtn.setEnabled(false); AudioLoaded = false;
        new Thread(() -> {
            try {
                byte[] Mp3 = ReadBytes(FileUri);
                UiHandler.post(() -> SetStatus("Decoding " + (Mp3.length / 1024) + " KB..."));
                String Result = NativeDecodeMp3(Mp3);
                if (Result.startsWith("ERROR:")) {
                    UiHandler.post(() -> { SetStatus("Failed: " + Result); PickBtn.setEnabled(true); });
                    return;
                }
                String[] Parts = Result.split(":");
                String Info = Parts[1] + " ch  " + Parts[2] + " Hz  " + FormatDur(Float.parseFloat(Parts[3]));
                AudioLoaded = true;
                UiHandler.post(() -> {
                    SetStatus("Ready: " + Info);
                    ConnectBtn.setEnabled(true); PickBtn.setEnabled(true);
                });
            } catch (Exception E) {
                UiHandler.post(() -> { SetStatus("Error: " + E.getMessage()); PickBtn.setEnabled(true); });
            }
        }).start();
    }
    private byte[] ReadBytes(Uri FileUri) throws Exception {
        InputStream Is = getContentResolver().openInputStream(FileUri);
        if (Is == null) throw new Exception("Cannot open file");
        ByteArrayOutputStream Out = new ByteArrayOutputStream();
        byte[] Buf = new byte[65536];
        int N;
        while ((N = Is.read(Buf)) != -1) Out.write(Buf, 0, N);
        Is.close();
        return Out.toByteArray();
    }
    private String FormatDur(float Secs) {
        return String.format("%d:%02d", (int) Secs / 60, (int) Secs % 60);
    }
    private Integer ReadTrimUs() {
        String Raw = TrimUsField.getText().toString().trim();
        if (Raw.isEmpty()) return 0;
        try {
            return Integer.parseInt(Raw);
        } catch (NumberFormatException E) {
            return null;
        }
    }
    private void StartEngine(String Ip, int TrimUs) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            getWindow().setSustainedPerformanceMode(true);
        }
        NativeSetOutputTrimUs(TrimUs);
        SetStatus("Syncing clock with " + Ip + "...");
        PickBtn.setEnabled(false); ConnectBtn.setEnabled(false); DisconnectBtn.setEnabled(true); TrimUsField.setEnabled(false);
        if (!WifiLock.isHeld()) WifiLock.acquire();
        if (!MulticastLock.isHeld()) MulticastLock.acquire();
        if (!WakeLock.isHeld()) WakeLock.acquire();
        new Thread(() -> {
            NativeConnect(Ip);
            UiHandler.post(() -> SetStatus("Armed. Waiting for fire signal from PC..."));
            NativeStartReceiveLoop();
            NativeDisconnect();
            ReleaseLocks();
            UiHandler.post(() -> {
                SetStatus("Disconnected.");
                PickBtn.setEnabled(true); ConnectBtn.setEnabled(AudioLoaded); DisconnectBtn.setEnabled(false); TrimUsField.setEnabled(true);
            });
        }) {{ setDaemon(true); }}.start();
    }
    private void StopEngine() {
        NativeDisconnect();
        ReleaseLocks();
    }
    private void ReleaseLocks() {
        if (WifiLock.isHeld()) WifiLock.release();
        if (MulticastLock.isHeld()) MulticastLock.release();
        if (WakeLock.isHeld()) WakeLock.release();
    }
    private void NativeConsoleLog(String Msg) {
        UiHandler.post(() -> AddConsoleLine(Msg));
    }
    private void AddConsoleLine(String Msg) {
        if (ConsoleView == null) return;
        ConsoleText.append(Msg).append('\n');
        if (ConsoleText.length() > MaxConsoleChars) ConsoleText.delete(0, ConsoleText.length() - MaxConsoleChars);
        ConsoleView.setText("Console:\n" + ConsoleText.toString());
        if (ConsoleScroll != null) ConsoleScroll.post(() -> ConsoleScroll.fullScroll(android.view.View.FOCUS_DOWN));
    }
    private void SetStatus(String Msg) {
        if (StatusView != null) StatusView.setText(Msg);
        AddConsoleLine("Status: " + Msg);
    }
    @Override protected void onDestroy() { StopEngine(); NativeClearConsoleSink(); super.onDestroy(); }
}
