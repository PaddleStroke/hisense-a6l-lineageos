import com.android.internal.telephony.NitzData;
import java.nio.file.*;
public class CheckNitz {
    public static void main(String[] args) throws Exception {
        var lines=Files.readAllLines(Path.of(args[0]));
        var missingAll=NitzData.parse(lines.get(0));
        var missingDst=NitzData.parse(lines.get(1));
        var known=NitzData.parse(lines.get(2));
        if(missingAll==null||missingDst==null||known==null)throw new AssertionError("Parser rejected fixture");
        if(missingAll.getLocalOffsetMillis()!=0 || missingAll.getDstAdjustmentMillis()!=0)throw new AssertionError();
        if(missingDst.getLocalOffsetMillis()!=7200000 || missingDst.getDstAdjustmentMillis()!=0)throw new AssertionError();
        var honestUnknownDst=NitzData.parse("26/09/28,12:00:00+8");
        if(honestUnknownDst==null||honestUnknownDst.getDstAdjustmentMillis()!=null)throw new AssertionError();
        if(known.getLocalOffsetMillis()!=7200000||known.getDstAdjustmentMillis()!=3600000)throw new AssertionError();
        if(missingAll.getCurrentTimeInMillis()!=known.getCurrentTimeInMillis())throw new AssertionError("Unexpected UTC conversion");
        System.out.println("F65 Android parser: missing timezone accepted as offset_ms=0; missing DST accepted as dst_ms=0");
        System.out.println("F65 omitted_DST_positive_control dst=null; known_fields_positive_control offset_ms=7200000 dst_ms=3600000; UTC_unchanged=1");
    }
}
