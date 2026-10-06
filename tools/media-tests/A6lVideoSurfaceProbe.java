import android.graphics.Canvas;
import android.graphics.Color;
import android.media.MediaCodec;
import android.media.MediaCodecInfo;
import android.media.MediaFormat;
import android.view.Surface;

/** Bounded off-screen probe of the persistent surface used by CameraX recording. */
public final class A6lVideoSurfaceProbe {
    public static void main(String[] args) {
        Thread deadline = new Thread(() -> {
            try { Thread.sleep(15000); } catch (InterruptedException ignored) { return; }
            System.err.println("VIDEO_SURFACE_TIMEOUT");
            System.exit(124);
        });
        deadline.setDaemon(true);
        deadline.start();
        Surface surface = null;
        MediaCodec codec = null;
        int exit = 1;
        try {
            surface = MediaCodec.createPersistentInputSurface();
            if (surface == null || !surface.isValid())
                throw new IllegalStateException("No valid persistent surface");
            System.out.println("VIDEO_PERSISTENT_SURFACE_PASS");
            codec = MediaCodec.createByCodecName("c2.android.avc.encoder");
            MediaFormat format = MediaFormat.createVideoFormat("video/avc", 320, 240);
            format.setInteger(MediaFormat.KEY_COLOR_FORMAT,
                    MediaCodecInfo.CodecCapabilities.COLOR_FormatSurface);
            format.setInteger(MediaFormat.KEY_BIT_RATE, 256000);
            format.setInteger(MediaFormat.KEY_FRAME_RATE, 5);
            format.setInteger(MediaFormat.KEY_I_FRAME_INTERVAL, 1);
            codec.configure(format, null, null, MediaCodec.CONFIGURE_FLAG_ENCODE);
            codec.setInputSurface(surface);
            codec.start();
            int[] colors = { Color.RED, Color.GREEN, Color.BLUE, Color.WHITE };
            for (int color : colors) {
                Canvas canvas = surface.lockCanvas(null);
                canvas.drawColor(color);
                surface.unlockCanvasAndPost(canvas);
                Thread.sleep(200);
            }
            codec.signalEndOfInputStream();
            MediaCodec.BufferInfo info = new MediaCodec.BufferInfo();
            long until = System.nanoTime() + 8000000000L;
            int samples = 0, bytes = 0;
            boolean eos = false;
            while (!eos && System.nanoTime() < until) {
                int index = codec.dequeueOutputBuffer(info, 10000);
                if (index == MediaCodec.INFO_OUTPUT_FORMAT_CHANGED) {
                    System.out.println("VIDEO_OUTPUT_FORMAT " + codec.getOutputFormat());
                } else if (index >= 0) {
                    if (info.size > 0 && (info.flags & MediaCodec.BUFFER_FLAG_CODEC_CONFIG) == 0) {
                        samples++;
                        bytes += info.size;
                    }
                    eos = (info.flags & MediaCodec.BUFFER_FLAG_END_OF_STREAM) != 0;
                    codec.releaseOutputBuffer(index, false);
                }
            }
            if (!eos || samples < 1 || bytes < 1)
                throw new IllegalStateException("No encoded frames/EOS: samples=" + samples
                        + " bytes=" + bytes + " eos=" + eos);
            System.out.println("VIDEO_SURFACE_ENCODE_PASS samples=" + samples + " bytes=" + bytes);
            exit = 0;
        } catch (Throwable failure) {
            failure.printStackTrace();
        } finally {
            if (codec != null) {
                try { codec.release(); } catch (Throwable ignored) { }
            }
            if (surface != null) surface.release();
            deadline.interrupt();
        }
        System.exit(exit);
    }
}
