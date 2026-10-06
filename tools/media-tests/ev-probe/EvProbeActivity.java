package org.a6l.evprobe;

import android.Manifest;
import android.app.Activity;
import android.content.pm.PackageManager;
import android.graphics.BitmapFactory;
import android.graphics.ImageFormat;
import android.graphics.Matrix;
import android.graphics.SurfaceTexture;
import android.hardware.camera2.CameraAccessException;
import android.hardware.camera2.CameraCaptureSession;
import android.hardware.camera2.CameraCharacteristics;
import android.hardware.camera2.CameraDevice;
import android.hardware.camera2.CameraManager;
import android.hardware.camera2.CaptureFailure;
import android.hardware.camera2.CaptureRequest;
import android.hardware.camera2.CaptureResult;
import android.hardware.camera2.TotalCaptureResult;
import android.hardware.camera2.params.StreamConfigurationMap;
import android.media.Image;
import android.media.ImageReader;
import android.os.Bundle;
import android.os.Environment;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.SystemClock;
import android.util.Range;
import android.util.Rational;
import android.util.Size;
import android.view.Surface;
import android.view.TextureView;
import android.widget.ArrayAdapter;
import android.widget.Button;
import android.widget.LinearLayout;
import android.widget.Spinner;
import android.widget.TextView;
import org.json.JSONArray;
import org.json.JSONObject;
import java.io.File;
import java.io.FileOutputStream;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.atomic.AtomicBoolean;

/** Attended two-photo probe. Camera opens only after the explicit Start action. */
public final class EvProbeActivity extends Activity implements TextureView.SurfaceTextureListener {
    private final Handler main = new Handler();
    private final AtomicBoolean active = new AtomicBoolean();
    private final ArrayList<String> ids = new ArrayList<>();
    private TextureView preview;
    private TextView status;
    private Spinner selector;
    private Button start;
    private CameraManager manager;
    private HandlerThread thread;
    private Handler work;
    private CameraDevice device;
    private CameraCaptureSession session;
    private ImageReader reader;
    private Surface previewSurface;
    private boolean used, awaitingPermission;
    private String cameraId;
    private Size jpegSize, previewSize;
    private int negativeStep, afMode, stage;
    private Rational compensationStep;
    private long stageStarted, imageTimestamp;
    private ProbePolicy policy;
    private byte[] imageBytes;
    private TotalCaptureResult stillResult;
    private boolean capturing;
    private File output;
    private JSONObject report;
    private JSONArray results;
    private static final class RequestTag {
        final int stage;
        final boolean still;
        RequestTag(int stage, boolean still) { this.stage = stage; this.still = still; }
        @Override public String toString() { return stage + ":" + (still ? "still" : "preview"); }
    }
    private final Runnable overallTimeout = () -> stop("overall 60-second deadline", false);
    private final Runnable stageTimeout = () -> {
        if (active.get() && !capturing) capture(true);
    };
    private final Runnable imageTimeout = () -> fail("JPEG/result pair timeout (8s)");

    @Override public void onCreate(Bundle saved) {
        super.onCreate(saved);
        LinearLayout layout = new LinearLayout(this);
        layout.setOrientation(LinearLayout.VERTICAL);
        status = new TextView(this);
        status.setText("Place a matte neutral/fine-letter target in the center, about 60 cm away. "
                + "Keep phone and lighting fixed. Start saves 0 and -1 EV photos; no manual exposure or tap AF.");
        selector = new Spinner(this);
        start = new Button(this);
        start.setText("Start one bounded EV test");
        preview = new TextureView(this);
        preview.setSurfaceTextureListener(this);
        layout.addView(status); layout.addView(selector); layout.addView(start);
        layout.addView(preview, new LinearLayout.LayoutParams(-1, 0, 1));
        setContentView(layout);
        manager = getSystemService(CameraManager.class);
        ArrayList<String> labels = new ArrayList<>();
        int preferred = -1;
        try {
            for (String id : manager.getCameraIdList()) {
                CameraCharacteristics c = manager.getCameraCharacteristics(id);
                Integer facing = c.get(CameraCharacteristics.LENS_FACING);
                int[] modes = c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES);
                boolean rear = facing != null && facing == CameraCharacteristics.LENS_FACING_BACK;
                boolean af = contains(modes, CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE);
                ids.add(id);
                labels.add("Camera " + id + " " + (rear ? "rear" : "front/other") + (af ? " / center AF" : " / fixed focus"));
                if (rear && (preferred < 0 || af)) preferred = ids.size() - 1;
            }
            selector.setAdapter(new ArrayAdapter<>(this, android.R.layout.simple_spinner_dropdown_item, labels));
            if (preferred >= 0) selector.setSelection(preferred);
            start.setEnabled(!ids.isEmpty());
        } catch (Exception e) { status.setText("Enumeration failed: " + e); start.setEnabled(false); }
        start.setOnClickListener(v -> {
            if (used || active.get()) return;
            if (checkSelfPermission(Manifest.permission.CAMERA) != PackageManager.PERMISSION_GRANTED) {
                awaitingPermission = true;
                requestPermissions(new String[]{Manifest.permission.CAMERA}, 1);
            } else begin();
        });
    }

    @Override public void onRequestPermissionsResult(int code, String[] permissions, int[] grants) {
        super.onRequestPermissionsResult(code, permissions, grants);
        if (code == 1 && awaitingPermission) {
            awaitingPermission = false;
            if (grants.length == 1 && grants[0] == PackageManager.PERMISSION_GRANTED) begin();
            else status.setText("Camera permission denied. No camera opened.");
        }
    }

    private static boolean contains(int[] values, int wanted) {
        if (values != null) for (int value : values) if (value == wanted) return true;
        return false;
    }

    private static Size choose(Size[] sizes) {
        Size best = null;
        if (sizes != null) for (Size s : sizes) {
            if (s.getWidth() <= 1600 && s.getHeight() <= 1200 && s.getWidth() > 0 && s.getHeight() > 0
                    && (best == null || (long)s.getWidth() * s.getHeight() > (long)best.getWidth() * best.getHeight())) best = s;
        }
        if (best == null) throw new IllegalArgumentException("No supported output within 1600x1200");
        return best;
    }

    private void begin() {
        if (used || !preview.isAvailable() || ids.isEmpty()) {
            status.setText("Preview surface not ready; press Start after it appears."); return;
        }
        used = true; start.setEnabled(false); selector.setEnabled(false);
        cameraId = ids.get(selector.getSelectedItemPosition());
        active.set(true);
        thread = new HandlerThread("A6lEvProbe"); thread.start(); work = new Handler(thread.getLooper());
        main.postDelayed(overallTimeout, 60000);
        work.post(() -> {
            try {
                CameraCharacteristics c = manager.getCameraCharacteristics(cameraId);
                Range<Integer> range = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_RANGE);
                compensationStep = c.get(CameraCharacteristics.CONTROL_AE_COMPENSATION_STEP);
                if (range == null || compensationStep == null) throw new IllegalArgumentException("No exposure compensation metadata");
                negativeStep = ProbePolicy.minusOneStep(range.getLower(), range.getUpper(), compensationStep.getNumerator(), compensationStep.getDenominator());
                if (!contains(c.get(CameraCharacteristics.CONTROL_AE_AVAILABLE_MODES), CaptureRequest.CONTROL_AE_MODE_ON))
                    throw new IllegalArgumentException("AE_ON unsupported");
                afMode = contains(c.get(CameraCharacteristics.CONTROL_AF_AVAILABLE_MODES), CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE)
                        ? CaptureRequest.CONTROL_AF_MODE_CONTINUOUS_PICTURE : CaptureRequest.CONTROL_AF_MODE_OFF;
                StreamConfigurationMap map = c.get(CameraCharacteristics.SCALER_STREAM_CONFIGURATION_MAP);
                if (map == null) throw new IllegalArgumentException("Missing stream configuration map");
                jpegSize = choose(map.getOutputSizes(ImageFormat.JPEG));
                Size[] previewSizes = map.getOutputSizes(SurfaceTexture.class);
                previewSize = choose(previewSizes);
                File external = getExternalFilesDir(Environment.DIRECTORY_PICTURES);
                if (external == null) throw new IllegalStateException("App-specific external storage unavailable");
                output = new File(external, "ev-test-" + System.currentTimeMillis());
                if (!output.mkdirs()) throw new IllegalStateException("Cannot create output directory");
                report = new JSONObject(); results = new JSONArray();
                report.put("camera_id", cameraId).put("compensation_range", range.toString()).put("compensation_step", compensationStep.toString())
                        .put("minus_one_requested_steps", negativeStep).put("minus_one_requested_ev", negativeStep * compensationStep.doubleValue())
                        .put("jpeg_size", jpegSize.toString()).put("preview_size", previewSize.toString()).put("af_mode", afMode)
                        .put("camera_characteristics", characteristics(c)).put("enumerated_camera_ids", new JSONArray(ids)).put("results", results)
                        .put("limits", "Camera2 result controls may be HAL echoes; not independent physical-frame readback. ISO is uncalibrated. Center AF only; no focus regions.");
                SurfaceTexture texture = preview.getSurfaceTexture();
                if (texture == null || !active.get()) throw new IllegalStateException("Preview disappeared");
                texture.setDefaultBufferSize(previewSize.getWidth(), previewSize.getHeight());
                previewSurface = new Surface(texture);
                main.post(() -> orientPreview(c));
                reader = ImageReader.newInstance(jpegSize.getWidth(), jpegSize.getHeight(), ImageFormat.JPEG, 2);
                reader.setOnImageAvailableListener(this::imageReady, work);
                manager.openCamera(cameraId, new CameraDevice.StateCallback() {
                    @Override public void onOpened(CameraDevice camera) {
                        if (!active.get()) { camera.close(); return; }
                        device = camera;
                        try {
                            camera.createCaptureSession(Arrays.asList(previewSurface, reader.getSurface()), new CameraCaptureSession.StateCallback() {
                                @Override public void onConfigured(CameraCaptureSession s) {
                                    if (!active.get()) { s.close(); return; }
                                    session = s; startStage(0);
                                }
                                @Override public void onConfigureFailed(CameraCaptureSession s) { s.close(); fail("Stream configuration failed"); }
                            }, work);
                        } catch (Exception e) { fail(e.toString()); }
                    }
                    @Override public void onDisconnected(CameraDevice camera) { camera.close(); fail("Camera disconnected"); }
                    @Override public void onError(CameraDevice camera, int error) { camera.close(); fail("Camera error " + error); }
                }, work);
            } catch (Exception e) { fail(e.toString()); }
        });
    }

    private JSONObject characteristics(CameraCharacteristics c) throws Exception {
        JSONObject j = new JSONObject();
        for (CameraCharacteristics.Key<?> key : c.getKeys()) {
            if (key.getName().contains("compensation") || key.getName().contains("sensor.info") || key.getName().contains("lens.info")
                    || key.getName().equals("android.sensor.orientation")) {
                Object value = c.get(key); j.put(key.getName(), describe(value));
            }
        }
        return j;
    }

    private static Object describe(Object value) {
        if (value == null) return JSONObject.NULL;
        if (value instanceof int[]) return Arrays.toString((int[])value);
        if (value instanceof float[]) return Arrays.toString((float[])value);
        return value.toString();
    }

    private void orientPreview(CameraCharacteristics c) {
        if (!active.get()) return;
        Integer sensor = c.get(CameraCharacteristics.SENSOR_ORIENTATION);
        int rotation = getDisplay() == null ? 0 : getDisplay().getRotation() * 90;
        int angle = ((sensor == null ? 0 : sensor) - rotation + 360) % 360;
        float width = preview.getWidth(), height = preview.getHeight();
        Matrix matrix = new Matrix();
        // Rotate the diagnostic preview; stored JPEG is deliberately native orientation (metadata records sensor orientation).
        matrix.postRotate(angle, width / 2, height / 2);
        preview.setTransform(matrix);
    }

    private int expectedStep() { return stage == 0 ? 0 : negativeStep; }

    private void apply(CaptureRequest.Builder request, boolean still) {
        request.set(CaptureRequest.CONTROL_MODE, CaptureRequest.CONTROL_MODE_AUTO);
        request.set(CaptureRequest.CONTROL_AE_MODE, CaptureRequest.CONTROL_AE_MODE_ON);
        request.set(CaptureRequest.CONTROL_AE_EXPOSURE_COMPENSATION, expectedStep());
        request.set(CaptureRequest.CONTROL_AF_MODE, afMode);
        request.setTag(new RequestTag(stage, still));
    }

    private void startStage(int next) {
        if (!active.get()) return;
        try {
            stage = next; capturing = false; imageBytes = null; stillResult = null; policy = new ProbePolicy();
            stageStarted = SystemClock.elapsedRealtime();
            CaptureRequest.Builder request = device.createCaptureRequest(CameraDevice.TEMPLATE_PREVIEW);
            apply(request, false); request.addTarget(previewSurface);
            session.setRepeatingRequest(request.build(), callback, work);
            work.removeCallbacks(stageTimeout); work.postDelayed(stageTimeout, ProbePolicy.MAX_SETTLE_MS);
            message("Stage " + (stage + 1) + "/2: " + (stage == 0 ? "0 EV" : "-1 EV") + ". Keep target and phone still.");
        } catch (Exception e) { fail(e.toString()); }
    }

    private final CameraCaptureSession.CaptureCallback callback = new CameraCaptureSession.CaptureCallback() {
        @Override public void onCaptureCompleted(CameraCaptureSession s, CaptureRequest request, TotalCaptureResult result) {
            if (!active.get()) return;
            try {
                if (!(request.getTag() instanceof RequestTag)) return;
                RequestTag tag = (RequestTag)request.getTag();
                if (tag.stage != stage) return;
                JSONObject row = new JSONObject().put("stage", stage).put("requested_steps", expectedStep())
                        .put("request_tag", request.getTag()).put("monotonic_ms", SystemClock.elapsedRealtime())
                        .put("frame_number", result.getFrameNumber());
                copyResult(row, result, CaptureResult.SENSOR_TIMESTAMP, "sensor_timestamp_ns");
                copyResult(row, result, CaptureResult.SENSOR_EXPOSURE_TIME, "exposure_ns");
                copyResult(row, result, CaptureResult.SENSOR_FRAME_DURATION, "sensor_frame_duration_ns");
                copyResult(row, result, CaptureResult.SENSOR_SENSITIVITY, "uncalibrated_iso");
                copyResult(row, result, CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION, "result_steps");
                copyResult(row, result, CaptureResult.CONTROL_AE_STATE, "ae_state");
                copyResult(row, result, CaptureResult.CONTROL_AF_STATE, "af_state");
                copyResult(row, result, CaptureResult.LENS_FOCUS_DISTANCE, "approximate_focus_dioptres");
                copyResult(row, result, CaptureResult.COLOR_CORRECTION_GAINS, "color_gains");
                boolean still = tag.still;
                row.put("still", still);
                if (results.length() < 240) results.put(row);
                if (still) {
                    report.put("stage" + stage + "_capture_result", row);
                    stillResult = result; pair(); return;
                }
                Integer ae = result.get(CaptureResult.CONTROL_AE_STATE);
                boolean converged = ae != null && (ae == CaptureResult.CONTROL_AE_STATE_CONVERGED || ae == CaptureResult.CONTROL_AE_STATE_LOCKED);
                Integer af = result.get(CaptureResult.CONTROL_AF_STATE);
                boolean afScanning = af != null && (af == CaptureResult.CONTROL_AF_STATE_PASSIVE_SCAN || af == CaptureResult.CONTROL_AF_STATE_ACTIVE_SCAN);
                long elapsed = SystemClock.elapsedRealtime() - stageStarted;
                if (!capturing && policy.shouldCapture(elapsed, expectedStep(), result.get(CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION), converged && !afScanning))
                    capture(elapsed >= ProbePolicy.MAX_SETTLE_MS);
            } catch (Exception e) { fail(e.toString()); }
        }
        @Override public void onCaptureFailed(CameraCaptureSession s, CaptureRequest request, CaptureFailure failure) {
            if (active.get()) fail("Capture failed: " + failure.getReason());
        }
    };

    private static <T> void copyResult(JSONObject row, CaptureResult result, CaptureResult.Key<T> key, String name) throws Exception {
        Object value = result.get(key);
        row.put(name, value == null ? JSONObject.NULL : value instanceof Number ? value : value.toString());
    }

    private void capture(boolean timeout) {
        if (!active.get() || capturing) return;
        capturing = true;
        work.removeCallbacks(stageTimeout);
        try {
            report.put("stage" + stage + "_settle_timeout", timeout);
            report.put("stage" + stage + "_settle_ms", SystemClock.elapsedRealtime() - stageStarted);
            CaptureRequest.Builder request = device.createCaptureRequest(CameraDevice.TEMPLATE_STILL_CAPTURE);
            apply(request, true); request.addTarget(reader.getSurface());
            request.set(CaptureRequest.JPEG_QUALITY, (byte)95);
            session.capture(request.build(), callback, work);
            work.postDelayed(imageTimeout, 8000);
        } catch (Exception e) { fail(e.toString()); }
    }

    private void imageReady(ImageReader images) {
        try (Image image = images.acquireNextImage()) {
            if (image == null || !active.get()) return;
            if (!capturing || imageBytes != null) throw new IllegalStateException("Unexpected/duplicate JPEG");
            ByteBuffer data = image.getPlanes()[0].getBuffer();
            if (data.remaining() <= 0 || data.remaining() > 16 * 1024 * 1024) throw new IllegalStateException("Invalid JPEG byte count");
            imageBytes = new byte[data.remaining()]; data.get(imageBytes); imageTimestamp = image.getTimestamp();
            pair();
        } catch (Exception e) { fail(e.toString()); }
    }

    private void pair() throws Exception {
        if (!active.get() || stillResult == null || imageBytes == null) return;
        Long timestamp = stillResult.get(CaptureResult.SENSOR_TIMESTAMP);
        Integer actual = stillResult.get(CaptureResult.CONTROL_AE_EXPOSURE_COMPENSATION);
        BitmapFactory.Options bounds = new BitmapFactory.Options(); bounds.inJustDecodeBounds = true;
        BitmapFactory.decodeByteArray(imageBytes, 0, imageBytes.length, bounds);
        ProbePolicy.validatePair(timestamp, imageTimestamp, actual, expectedStep(), bounds.outWidth, bounds.outHeight,
                jpegSize.getWidth(), jpegSize.getHeight());
        String name = stage == 0 ? "0ev.jpg" : "minus1ev.jpg";
        write(new File(output, name), imageBytes);
        report.put("stage" + stage + "_image_timestamp_ns", imageTimestamp).put("stage" + stage + "_jpeg_bytes", imageBytes.length)
                .put("stage" + stage + "_file", name);
        work.removeCallbacks(imageTimeout);
        if (stage == 0) startStage(1); else stop("Both photos saved", true);
    }

    private static void write(File file, byte[] bytes) throws Exception {
        try (FileOutputStream out = new FileOutputStream(file)) { out.write(bytes); }
    }

    private void message(String text) { main.post(() -> status.setText(text)); }
    private void fail(String reason) { stop(reason, false); }

    private void stop(String reason, boolean success) {
        if (!active.getAndSet(false)) return;
        main.removeCallbacks(overallTimeout);
        Handler handler = work;
        if (handler == null) return;
        handler.post(() -> {
            handler.removeCallbacks(stageTimeout); handler.removeCallbacks(imageTimeout);
            try {
                if (session != null) session.close();
                if (device != null) device.close();
                if (reader != null) reader.close();
                if (previewSurface != null) previewSurface.release();
                if (report != null && output != null) {
                    report.put("success", success).put("finish_reason", reason).put("finish_monotonic_ms", SystemClock.elapsedRealtime());
                    write(new File(output, "report.json"), report.toString(2).getBytes(StandardCharsets.UTF_8));
                }
                message(reason + (output == null ? "" : "\nSaved in " + output) + "\nCamera closed. Reopen app for another test.");
            } catch (Exception e) { message("Cleanup/report error: " + e); }
            finally { thread.quitSafely(); }
        });
    }

    @Override protected void onPause() {
        awaitingPermission = false;
        stop("Activity backgrounded; test cancelled", false);
        super.onPause();
    }
    @Override protected void onDestroy() { stop("Activity destroyed", false); super.onDestroy(); }
    @Override public void onSurfaceTextureAvailable(SurfaceTexture texture, int width, int height) { }
    @Override public void onSurfaceTextureSizeChanged(SurfaceTexture texture, int width, int height) { }
    @Override public boolean onSurfaceTextureDestroyed(SurfaceTexture texture) { stop("Preview surface destroyed", false); return true; }
    @Override public void onSurfaceTextureUpdated(SurfaceTexture texture) { }
}
