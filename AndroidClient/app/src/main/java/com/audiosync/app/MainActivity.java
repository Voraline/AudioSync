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
import android.view.Gravity;
import android.view.ViewGroup;
import android.widget.*;
import java.io.ByteArrayOutputStream;
import java.io.InputStream;
public class MainActivity extends Activity {
    static { System.loadLibrary("audiosync"); }
    public native String NativeDecodeMp3(byte[] Mp3Data);
    public native void NativeConnect(String Ip);
    public native void NativeStartReceiveLoop();
    public native void NativeDisconnect();
    private static final int PickRequest = 1;
    private Handler UiHandler;
    private TextView StatusView;
    private Button PickBtn, ConnectBtn, DisconnectBtn;
    private EditText IpField;
    private boolean AudioLoaded = false;
    private WifiManager.WifiLock WifiLock;
    private PowerManager.WakeLock WakeLock;
    @Override
    protected void onCreate(Bundle S) {
        super.onCreate(S);
        setVolumeControlStream(AudioManager.STREAM_MUSIC);
        UiHandler = new Handler(Looper.getMainLooper());
        WifiManager Wm = (WifiManager) getApplicationContext().getSystemService(WIFI_SERVICE);
        WifiLock = Wm.createWifiLock(WifiManager.WIFI_MODE_FULL_LOW_LATENCY, "AudioSync:Wifi");
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
        ConnectBtn = new Button(this); ConnectBtn.setText("CONNECT & ARM");
        ConnectBtn.setLayoutParams(WithMargin); ConnectBtn.setEnabled(false);
        DisconnectBtn = new Button(this); DisconnectBtn.setText("DISCONNECT");
        DisconnectBtn.setLayoutParams(WithMargin); DisconnectBtn.setEnabled(false);
        StatusView = new TextView(this); StatusView.setText("Pick an MP3 file to begin.");
        StatusView.setGravity(Gravity.CENTER); StatusView.setPadding(0, 32, 0, 0);
        Root.addView(Title); Root.addView(PickBtn); Root.addView(IpField);
        Root.addView(ConnectBtn); Root.addView(DisconnectBtn); Root.addView(StatusView);
        setContentView(Root);
        PickBtn.setOnClickListener(V -> {
            Intent I = new Intent(Intent.ACTION_GET_CONTENT); I.setType("audio/mpeg");
            startActivityForResult(I, PickRequest);
        });
        ConnectBtn.setOnClickListener(V -> {
            String Ip = IpField.getText().toString().trim();
            if (Ip.isEmpty()) { SetStatus("Enter the PC IP address."); return; }
            StartEngine(Ip);
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
    private void StartEngine(String Ip) {
        if (android.os.Build.VERSION.SDK_INT >= android.os.Build.VERSION_CODES.N) {
            getWindow().setSustainedPerformanceMode(true);
        }
        SetStatus("Syncing clock with " + Ip + "...");
        PickBtn.setEnabled(false); ConnectBtn.setEnabled(false); DisconnectBtn.setEnabled(true);
        if (!WifiLock.isHeld()) WifiLock.acquire();
        if (!WakeLock.isHeld()) WakeLock.acquire();
        new Thread(() -> {
            NativeConnect(Ip);
            UiHandler.post(() -> SetStatus("Armed. Waiting for fire signal from PC..."));
            NativeStartReceiveLoop();
            UiHandler.post(() -> {
                SetStatus("Disconnected.");
                PickBtn.setEnabled(true); ConnectBtn.setEnabled(AudioLoaded); DisconnectBtn.setEnabled(false);
            });
        }) {{ setDaemon(true); }}.start();
    }
    private void StopEngine() {
        NativeDisconnect();
        if (WifiLock.isHeld()) WifiLock.release();
        if (WakeLock.isHeld()) WakeLock.release();
    }
    private void SetStatus(String Msg) { if (StatusView != null) StatusView.setText(Msg); }
    @Override protected void onDestroy() { super.onDestroy(); StopEngine(); }
}
