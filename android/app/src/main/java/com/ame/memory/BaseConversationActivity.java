package com.ame.memory;

import android.net.Uri;
import android.os.Bundle;
import android.util.Log;
import android.view.View;
import android.widget.EditText;
import android.widget.ImageView;
import android.widget.TextView;
import android.widget.Toast;

import androidx.activity.result.ActivityResultLauncher;
import androidx.activity.result.PickVisualMediaRequest;
import androidx.activity.result.contract.ActivityResultContracts;
import androidx.appcompat.app.AppCompatActivity;
import androidx.core.content.FileProvider;
import androidx.recyclerview.widget.LinearLayoutManager;
import androidx.recyclerview.widget.RecyclerView;

import org.json.JSONObject;

import java.io.File;
import java.io.IOException;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;

/** Shared conversation UI. Engine creation, calls and closure stay on one IO queue. */
abstract class BaseConversationActivity extends AppCompatActivity {
    public static final String EXTRA_SESSION_ID = "session_id";
    public static final String EXTRA_SESSION_NAME = "session_name";

    private ConversationWorkQueue io;
    private final List<Message> messages = new ArrayList<>();
    private List<MemoryOrchestrator.TurnDiag> diagnostics = new ArrayList<>();
    private AmeEngine engine;
    private MemoryOrchestrator orchestrator;
    private SessionStore store;
    private String sessionId;
    private File sessionDir;
    private MessageAdapter adapter;
    private RecyclerView messageList;
    private RecyclerView diagnosticList;
    private TextView statusView;
    private boolean diagnosticMode;
    private boolean ready;
    private volatile boolean destroyed;
    private boolean webOn;
    private boolean deepOn;
    private String pendingImageB64;
    private File cameraFile;
    private Uri cameraOut;

    private final ActivityResultLauncher<PickVisualMediaRequest> pickImage =
            registerForActivityResult(new ActivityResultContracts.PickVisualMedia(),
                    this::acceptImage);
    private final ActivityResultLauncher<Uri> takePicture =
            registerForActivityResult(new ActivityResultContracts.TakePicture(),
                    this::acceptCameraResult);

    protected abstract int conversationLayout();

    protected String defaultSessionName() {
        return "会话";
    }

    protected void configureSession(String name) {
        setTitle(name);
    }

    protected MemoryOrchestrator createOrchestrator(AmeEngine opened, SessionStore sessions,
                                                   String id) {
        return new MemoryOrchestrator(opened, sessions, id);
    }

    protected final File sessionDirectory() {
        return sessionDir;
    }

    @Override
    protected void onCreate(Bundle savedInstanceState) {
        super.onCreate(savedInstanceState);
        setContentView(conversationLayout());
        sessionId = getIntent().getStringExtra(EXTRA_SESSION_ID);
        if (sessionId == null) {
            finish();
            return;
        }
        String name = getIntent().getStringExtra(EXTRA_SESSION_NAME);
        store = new SessionStore(getFilesDir());
        sessionDir = store.sessionDir(sessionId);
        configureSession(name == null ? defaultSessionName() : name);
        bindMessageLists();
        bindActions();
        openWorkQueue();
    }

    private void bindMessageLists() {
        statusView = findViewById(R.id.tvStatus);
        messages.addAll(store.loadMessages(sessionId));
        adapter = new MessageAdapter(messages);
        messageList = findViewById(R.id.rvMessages);
        LinearLayoutManager layout = new LinearLayoutManager(this);
        layout.setStackFromEnd(true);
        messageList.setLayoutManager(layout);
        messageList.setAdapter(adapter);
        diagnosticList = findViewById(R.id.rvDiag);
        diagnosticList.setLayoutManager(new LinearLayoutManager(this));
    }

    private void bindActions() {
        EditText input = findViewById(R.id.etInput);
        findViewById(R.id.btnPick).setOnClickListener(v -> pickImage.launch(
                new PickVisualMediaRequest.Builder()
                        .setMediaType(ActivityResultContracts.PickVisualMedia.ImageOnly.INSTANCE)
                        .build()));
        findViewById(R.id.btnCamera).setOnClickListener(v -> launchCamera());
        findViewById(R.id.btnClearImg).setOnClickListener(v -> clearPending());
        findViewById(R.id.btnWeb).setOnClickListener(this::toggleWeb);
        findViewById(R.id.btnDeep).setOnClickListener(this::toggleDeep);
        findViewById(R.id.btnDiag).setOnClickListener(v -> toggleDiagnostics());
        findViewById(R.id.btnSend).setOnClickListener(v -> sendMessage(input));
    }

    private void openWorkQueue() {
        try {
            io = ConversationWorkQueue.acquire(sessionDir);
            io.execute(this::initializeEngine);
        } catch (IOException error) {
            Log.e(MemoryOrchestrator.TAG, "Unable to open session directory; error_type="
                    + error.getClass().getSimpleName());
            showFailure("记忆加载失败: ", error);
        }
    }

    private void initializeEngine() {
        if (destroyed) return;
        try {
            engine = AmeEngine.create(io.directory());
            KeywordDict.ensureLoaded(this, engine);
            CheapRerank.ensureLoaded(this, engine);
            orchestrator = createOrchestrator(engine, store, sessionId);
            runOnUiThread(this::showEngineReady);
        } catch (Exception error) {
            closeEngine();
            Log.e(MemoryOrchestrator.TAG, "[" + sessionId + "] Memory initialization failed; error_type="
                    + error.getClass().getSimpleName());
            runOnUiThread(() -> showFailure("记忆加载失败: ", error));
        }
    }

    private void showEngineReady() {
        if (destroyed) return;
        ready = true;
        diagnosticList.setAdapter(new DiagAdapter(diagnostics));
        refreshStatus();
    }

    private boolean ensureReady() {
        if (destroyed) return false;
        if (ready) return true;
        Toast.makeText(this, "记忆加载中，请稍候…", Toast.LENGTH_SHORT).show();
        return false;
    }

    private void toggleWeb(View button) {
        webOn = !webOn;
        button.setBackgroundColor(webOn ? 0xFFDCE3FF : 0xFFFFFFFF);
    }

    private void toggleDeep(View button) {
        deepOn = !deepOn;
        button.setBackgroundColor(deepOn ? 0xFFDCE3FF : 0xFFFFFFFF);
    }

    private void sendMessage(EditText input) {
        if (!ensureReady()) return;
        String text = input.getText().toString().trim();
        String image = pendingImageB64;
        if (text.isEmpty() && image == null) return;
        boolean search = webOn;
        boolean rerank = deepOn;
        input.getText().clear();
        clearPending();
        appendLocal(new Message("user", text.isEmpty() ? "[图片]" : text,
                System.currentTimeMillis() / 1000, null));
        Message pending = new Message("assistant", "思考中…", 0, null);
        appendLocal(pending);
        io.execute(() -> requestReply(text, image, search, rerank, pending));
    }

    private void requestReply(String text, String image, boolean search, boolean rerank,
                              Message pending) {
        if (destroyed) return;
        try {
            String reply = orchestrator.chat(text, image, null, search, rerank);
            runOnUiThread(() -> completeReply(pending, reply));
        } catch (Exception error) {
            Log.e(MemoryOrchestrator.TAG, "[" + sessionId + "] Request failed; error_type="
                    + error.getClass().getSimpleName());
            runOnUiThread(() -> failReply(pending, error));
        }
    }

    private void completeReply(Message pending, String reply) {
        if (destroyed) return;
        int index = messages.indexOf(pending);
        if (index < 0) return;
        messages.set(index, new Message("assistant", reply, 0, null));
        adapter.notifyItemChanged(index);
        refreshStatus();
    }

    private void failReply(Message pending, Exception error) {
        if (destroyed) return;
        int index = messages.indexOf(pending);
        if (index >= 0) {
            messages.remove(index);
            adapter.notifyItemRemoved(index);
        }
        showFailure("请求失败: ", error);
        refreshStatus();
    }

    private void appendLocal(Message message) {
        messages.add(message);
        adapter.notifyItemInserted(messages.size() - 1);
        messageList.scrollToPosition(messages.size() - 1);
    }

    private void toggleDiagnostics() {
        if (!ensureReady()) return;
        diagnosticMode = !diagnosticMode;
        messageList.setVisibility(diagnosticMode ? View.GONE : View.VISIBLE);
        diagnosticList.setVisibility(diagnosticMode ? View.VISIBLE : View.GONE);
        if (!diagnosticMode) return;
        diagnosticList.setAdapter(new DiagAdapter(diagnostics));
        if (diagnostics.isEmpty()) {
            Toast.makeText(this, "本轮会话还没有对话记录", Toast.LENGTH_SHORT).show();
        }
        refreshStatus();
    }

    private void refreshStatus() {
        if (destroyed || !ready) return;
        io.execute(this::loadStatus);
    }

    private void loadStatus() {
        if (destroyed) return;
        try {
            JSONObject stats = new JSONObject(orchestrator.statsJson());
            String line = formatStatus(stats, orchestrator.getLastSentTokens());
            // Snapshot diagnostics on the same queue that writes the orchestrator log.
            List<MemoryOrchestrator.TurnDiag> snapshot = orchestrator.diag();
            runOnUiThread(() -> showStatus(line, snapshot));
        } catch (Exception error) {
            Log.w(MemoryOrchestrator.TAG, "Unable to read statistics; retaining previous status; error_type="
                    + error.getClass().getSimpleName());
        }
    }

    private static String formatStatus(JSONObject stats, int sent) {
        long cumulative = stats.optLong("total_chars") / 2;
        String ratio = (sent > 0 && cumulative > 0)
                ? String.format("%.1f×", (double) cumulative / sent) : "—";
        return "记忆 " + stats.optLong("turns") + "轮 ~" + cumulative + "tok │ 发送 " + sent
                + "tok │ 压缩 " + ratio + " │ 节点 " + stats.optLong("nodes")
                + " │ 窗口" + BuildConfig.CONTEXT_WINDOW_TURNS
                + "+证据" + BuildConfig.RECALL_TOP_K;
    }

    private void showStatus(String line, List<MemoryOrchestrator.TurnDiag> snapshot) {
        if (destroyed) return;
        statusView.setText(line);
        diagnostics = snapshot;
        if (diagnosticMode) diagnosticList.setAdapter(new DiagAdapter(diagnostics));
    }

    private void acceptImage(Uri uri) {
        if (destroyed || uri == null) return;
        try {
            ConversationImage image = ConversationImage.read(getContentResolver(), uri);
            pendingImageB64 = image.base64;
            ((ImageView) findViewById(R.id.ivPending)).setImageBitmap(image.preview);
            findViewById(R.id.rowPending).setVisibility(View.VISIBLE);
        } catch (Exception error) {
            showFailure("图片处理失败: ", error);
        }
    }

    private void launchCamera() {
        deleteCameraFile();
        try {
            cameraFile = File.createTempFile("cam_", ".jpg", getCacheDir());
            cameraOut = FileProvider.getUriForFile(this, "com.ame.memory.fileprovider", cameraFile);
            takePicture.launch(cameraOut);
        } catch (Exception error) {
            deleteCameraFile();
            showFailure("无法启动相机: ", error);
        }
    }

    private void acceptCameraResult(boolean success) {
        if (success) acceptImage(cameraOut);
        deleteCameraFile();
    }

    private void clearPending() {
        pendingImageB64 = null;
        deleteCameraFile();
        ((ImageView) findViewById(R.id.ivPending)).setImageDrawable(null);
        findViewById(R.id.rowPending).setVisibility(View.GONE);
    }

    private void deleteCameraFile() {
        if (cameraFile == null) return;
        try {
            Files.deleteIfExists(cameraFile.toPath());
        } catch (IOException error) {
            Log.w(MemoryOrchestrator.TAG, "Unable to delete temporary camera file; error_type="
                    + error.getClass().getSimpleName());
        }
        cameraFile = null;
        cameraOut = null;
    }

    private void showFailure(String prefix, Exception error) {
        if (destroyed) return;
        Toast.makeText(this, prefix + error.getMessage(), Toast.LENGTH_LONG).show();
    }

    private void closeEngine() {
        if (engine == null) return;
        AmeEngine closing = engine;
        engine = null;
        orchestrator = null;
        closing.close();
    }

    @Override
    protected void onDestroy() {
        destroyed = true;
        ready = false;
        deleteCameraFile();
        // A running native call must finish before its engine is released. Queued
        // requests observe destroyed and skip work; late UI callbacks do likewise.
        if (io != null) io.close(this::closeEngine);
        super.onDestroy();
    }
}
