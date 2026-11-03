package com.ame.memory;

import android.content.ContentResolver;
import android.graphics.Bitmap;
import android.graphics.BitmapFactory;
import android.net.Uri;
import android.util.Base64;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;

/** Images remain in memory: longest side 1024 pixels, JPEG quality 80. */
final class ConversationImage {
    final Bitmap preview;
    final String base64;

    private ConversationImage(Bitmap preview, String base64) {
        this.preview = preview;
        this.base64 = base64;
    }

    static ConversationImage read(ContentResolver resolver, Uri uri) throws IOException {
        BitmapFactory.Options bounds = new BitmapFactory.Options();
        bounds.inJustDecodeBounds = true;
        decode(resolver, uri, bounds);
        if (bounds.outWidth <= 0 || bounds.outHeight <= 0) throw new IOException("无法解码图片");
        BitmapFactory.Options options = new BitmapFactory.Options();
        options.inSampleSize = sampleSize(Math.max(bounds.outWidth, bounds.outHeight));
        Bitmap raw = decode(resolver, uri, options);
        if (raw == null) throw new IOException("无法解码图片");
        Bitmap preview = resize(raw);
        ByteArrayOutputStream output = new ByteArrayOutputStream();
        if (!preview.compress(Bitmap.CompressFormat.JPEG, 80, output)) {
            throw new IOException("无法压缩图片");
        }
        return new ConversationImage(preview,
                Base64.encodeToString(output.toByteArray(), Base64.NO_WRAP));
    }

    private static Bitmap decode(ContentResolver resolver, Uri uri, BitmapFactory.Options options)
            throws IOException {
        try (InputStream input = resolver.openInputStream(uri)) {
            if (input == null) throw new IOException("无法读取图片");
            return BitmapFactory.decodeStream(input, null, options);
        }
    }

    private static int sampleSize(int longestSide) {
        int sample = 1;
        while (longestSide / (sample * 2) >= 1024) sample *= 2;
        return sample;
    }

    private static Bitmap resize(Bitmap raw) {
        float scale = 1024f / Math.max(raw.getWidth(), raw.getHeight());
        if (scale >= 1f) return raw;
        return Bitmap.createScaledBitmap(raw, Math.max(1, (int) (raw.getWidth() * scale)),
                Math.max(1, (int) (raw.getHeight() * scale)), true);
    }
}
