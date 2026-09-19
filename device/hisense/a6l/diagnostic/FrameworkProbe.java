package org.a6l.probe;

/** Offline ART/framework JNI exercise; no hardware or persistent storage. */
public final class FrameworkProbe {
    public static void main(String[] args) throws Exception {
        System.out.println("A6L_ART_JAVA_ENTER vm=" + System.getProperty("java.vm.version"));
        if (args.length != 0 && args[0].equals("art")) {
            byte[] digest = java.security.MessageDigest.getInstance("SHA-256")
                    .digest("A6L runtime".getBytes(java.nio.charset.StandardCharsets.UTF_8));
            if (digest.length != 32) throw new AssertionError("digest");
            System.out.println("A6L_ART_CORE_PASS digest=32");
            return;
        }
        Class<?> clock = Class.forName("android.os.SystemClock");
        long uptime = (Long) clock.getMethod("uptimeMillis").invoke(null);
        Class<?> process = Class.forName("android.os.Process");
        int uid = (Integer) process.getMethod("myUid").invoke(null);
        Class<?> parcel = Class.forName("android.os.Parcel");
        Object p = parcel.getMethod("obtain").invoke(null);
        parcel.getMethod("writeInt", int.class).invoke(p, 0x41364c);
        parcel.getMethod("setDataPosition", int.class).invoke(p, 0);
        if ((Integer) parcel.getMethod("readInt").invoke(p) != 0x41364c)
            throw new AssertionError("native Parcel roundtrip");
        parcel.getMethod("recycle").invoke(p);
        Class<?> shared = Class.forName("android.os.SharedMemory");
        Object region = shared.getMethod("create", String.class, int.class)
                .invoke(null, "a6l-framework-probe", 4096);
        java.nio.ByteBuffer memory = (java.nio.ByteBuffer) shared.getMethod("mapReadWrite").invoke(region);
        memory.putInt(0, 0x41364c);
        if (memory.getInt(0) != 0x41364c) throw new AssertionError("shared memory roundtrip");
        shared.getMethod("unmap", java.nio.ByteBuffer.class).invoke(null, memory);
        if (!(Boolean) shared.getMethod("setProtect", int.class).invoke(region, 1))
            throw new AssertionError("shared memory read-only protection");
        shared.getMethod("close").invoke(region);
        System.out.println("A6L_SHARED_MEMORY_PASS bytes=4096 protection=read-only");
        Class<?> services = Class.forName("android.os.ServiceManager");
        if (services.getMethod("checkService", String.class).invoke(null, "manager") == null)
            throw new AssertionError("Binder service manager unavailable");
        System.out.println("A6L_FRAMEWORK_BINDER_PASS service=manager");
        System.out.println("A6L_FRAMEWORK_JNI_PASS uid=" + uid + " uptime=" + uptime);
    }
}
