package com.ame.memory;

import android.app.AlertDialog;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.util.Log;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;
import android.widget.Toast;

import java.io.File;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Comparator;
import java.util.List;

/** Guide-specific prompt and plan viewer; conversation behavior lives in the base class. */
public class GuideActivity extends BaseConversationActivity {
    private static final int PLAN_DIALOG_MAX_IMGS = 20;
    private String guideName;
    private String guideText;

    @Override
    protected int conversationLayout() {
        return R.layout.activity_guide;
    }

    @Override
    protected String defaultSessionName() {
        return "导览";
    }

    @Override
    protected void configureSession(String name) {
        setTitle("导游 · " + name);
        guideName = name;
        guideText = readGuideText();
        if (guideText == null) {
            Toast.makeText(this, "未找到导览方案文件，按普通对话模式运行",
                    Toast.LENGTH_LONG).show();
        }
        findViewById(R.id.btnPlan).setOnClickListener(v -> showPlanDialog(name));
    }

    @Override
    protected MemoryOrchestrator createOrchestrator(AmeEngine opened, SessionStore sessions,
                                                   String id) {
        return new MemoryOrchestrator(opened, sessions, id,
                BuildConfig.CONTEXT_WINDOW_TURNS, BuildConfig.RECALL_TOP_K, guideName, guideText);
    }

    private String readGuideText() {
        File guide = new File(sessionDirectory(), "guide.txt");
        if (!guide.exists()) return null;
        try {
            return new String(Files.readAllBytes(guide.toPath()), StandardCharsets.UTF_8);
        } catch (IOException error) {
            Log.w(MemoryOrchestrator.TAG, "读取导览方案失败，继续展示可用内容", error);
            return null;
        }
    }

    // showPlanDialog：ScrollView（方案全文 + 方案图片纵向排列）；图片上限 PLAN_DIALOG_MAX_IMGS 张。
    private void showPlanDialog(String sessionName) {
        ScrollView scroll = new ScrollView(this);
        LinearLayout col = new LinearLayout(this);
        col.setOrientation(LinearLayout.VERTICAL);
        int pad = (int) (12 * getResources().getDisplayMetrics().density);
        col.setPadding(pad, pad, pad, pad);
        String guide = readGuideText();
        TextView tv = new TextView(this);
        tv.setText(guide != null ? guide : "（无方案文本）");
        tv.setTextSize(14f);
        col.addView(tv, new LinearLayout.LayoutParams(
                LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        List<File> plan = planImages();
        int limit = Math.min(PLAN_DIALOG_MAX_IMGS, plan.size());
        for (int i = 0; i < limit; i++) {
            File f = plan.get(i);
            ImageView iv = new ImageView(this);
            iv.setImageBitmap(decodePlanImage(f));
            iv.setAdjustViewBounds(true);
            col.addView(iv, new LinearLayout.LayoutParams(
                    LinearLayout.LayoutParams.MATCH_PARENT, LinearLayout.LayoutParams.WRAP_CONTENT));
        }
        scroll.addView(col);
        new AlertDialog.Builder(this)
                .setTitle("导览方案 · " + sessionName)
                .setView(scroll)
                .setPositiveButton("关闭", null)
                .show();
    }

    private static Bitmap decodePlanImage(File image) {
        try {
            return BitmapFactory.decodeFile(image.getCanonicalPath());
        } catch (IOException error) {
            Log.w(MemoryOrchestrator.TAG, "读取方案图片失败，继续展示其他内容", error);
            return null;
        }
    }

    /** 方案图片：优先 PDF 内嵌图目录，其次页渲染图目录 */
    private List<File> planImages() {
        File imgs = new File(sessionDirectory(), "guide_img");
        File pages = new File(sessionDirectory(), "guide_page");
        File[] imgJpg = imgs.listFiles((dir, name) -> name.endsWith(".jpg"));
        boolean hasEmbedded = imgJpg != null && imgJpg.length > 0;
        File from = hasEmbedded ? imgs : pages;
        File[] jpg = from.listFiles((dir, name) -> name.endsWith(".jpg"));
        if (jpg == null) return new ArrayList<>();
        List<File> out = new ArrayList<>(Arrays.asList(jpg));
        out.sort(Comparator.comparing(f -> f.getName()));
        return out;
    }

}
