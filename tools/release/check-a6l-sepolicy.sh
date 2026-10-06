#!/usr/bin/env bash
# Offline A6L vendor sepolicy check (release-prep, 27 Sep 2026). NO build, NO `m`: re-runs the same steps soong runs for
# vendor_sepolicy.cil (m4 -> checkpolicy -> filter_out reqd mask -> version_policy) on the file list of the LAST built
# vendor_sepolicy.conf, with the device/hisense/a6l files taken from THIS repo and extra dirs added, then links the whole
# split policy with secilc (plat + system_ext + product + mappings + vendor) = type/attribute resolution + ALL neverallows,
# and validates the vendor file_contexts against the linked policy with sefcontext_compile.
# usage (WSL, as a6l):  bash tools/release/check-a6l-sepolicy.sh [userdebug|user] [extra device-relative sepolicy dirs...]
#   e.g.  bash tools/release/check-a6l-sepolicy.sh user hals/sepolicy rom/sepolicy/vendor
# Needs a previous build's intermediates in out/soong/.intermediates/system/sepolicy (r3 has them). Prints A6L_SEPOLICY_CHECK PASS|FAIL.
set -uo pipefail
VAR=${1:-userdebug}; shift || true; EXTRA=("$@")
R=${A6L_REPO:-/mnt/c/Users/Pierre/Desktop/A6L}; L=${A6L_TREE:-/home/a6l/android/a6l-lineage24}
S=$L/out/soong/.intermediates/system/sepolicy; H=$L/out/host/linux-x86/bin
W=${A6L_SEPCHECK_DIR:-/tmp/a6l-sepcheck-$VAR}; rm -rf $W; mkdir -p $W
CONF=$S/vendor_sepolicy.conf/android_common/a6l/vendor_sepolicy.conf
for f in $CONF $H/checkpolicy $H/secilc $H/version_policy $H/build_sepolicy $H/sefcontext_compile; do [ -e $f ] || { echo "missing $f"; echo "A6L_SEPOLICY_CHECK FAIL (tools)"; exit 1; }; done
( cd $L && python3 - "$CONF" "$R" "$W/files.txt" "${EXTRA[@]}" ) <<'PY'
import re, sys, os, glob
conf, repo, out, extra = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4:]
seen, marked = set(), []
for ln in open(conf, errors='replace'):
    m = re.match(r'#line 1 "(.*)"$', ln.rstrip('\n'))
    if m and m.group(1) not in seen and os.path.basename(m.group(1)) != 'newline':
        seen.add(m.group(1)); marked.append(m.group(1))
# m4 -s only marks files that emit text, so macro-only files (global_macros, te_macros, ...) are not in the markers:
# rebuild the list the way soong's se_build_files does: for each kind (fixed order), every policy dir (build order).
# = policyConfOrder in system/sepolicy/build/soong/policy.go
KINDS = ['flagging_macros', 'security_classes', 'initial_sids', 'access_vectors', 'global_macros', 'neverallow_macros',
         'mls_macros', 'mls_decl', 'mls', 'policy_capabilities', 'te_macros', 'ioctl_defines', 'ioctl_macros',
         'nlmsg_defines', 'nlmsg_macros', 'attributes|*.te', 'roles_decl', 'roles', 'users', 'initial_sid_contexts',
         'fs_use', 'genfs_contexts', 'port_contexts']
use_tree = os.environ.get('A6L_USE_TREE') == '1'
def repo_path(p):
    return os.path.join(repo, p) if (p.startswith('device/hisense/a6l/') and not use_tree) else p
# directory order = order of the attributes/*.te markers (the group where dir order is visible), then the rest
dirs = []
for p in [m for m in marked if m.endswith('.te') or m.endswith('/attributes')] + marked:
    d = os.path.dirname(p)
    if d not in dirs: dirs.append(d)
dirs = ['system/sepolicy/flagging'] + [repo_path(d) for d in dirs]
# completeness audit (29 Sep 2026): an extra dir the last build already compiled (e.g. rom/sepolicy/vendor in an
# A6L_SEPOLICY_ROM=1 build such as r5) is not added twice (checkpolicy: 'Duplicate declaration of type')
dirs += [x for x in (os.path.join(repo, 'device/hisense/a6l', e.rstrip('/')) for e in extra) if x not in dirs]
files = []
for k in KINDS:
    for d in dirs:
        if k == 'attributes|*.te':
            files += ([os.path.join(d, 'attributes')] if os.path.isfile(os.path.join(d, 'attributes')) else []) + sorted(glob.glob(os.path.join(d, '*.te')))
        elif os.path.isfile(os.path.join(d, k)): files.append(os.path.join(d, k))
nl = os.path.join(os.path.dirname(out), 'newline'); open(nl, 'w').write('\n')
files = [x for f in files for x in (f, nl)]
new_te = [f for f in files if f.endswith('.te') and (f.startswith(repo) and repo_path(os.path.relpath(f, repo)) not in [repo_path(m) for m in marked])]
new_genfs = []
missing = [p for p in files if not os.path.exists(p)]
if missing: print('MISSING', missing); sys.exit(1)
open(out, 'w').write('\n'.join(files) + '\n')
print('policy files:', len(files), ' added .te:', [os.path.relpath(p, repo) for p in new_te], ' added genfs:', [os.path.relpath(p, repo) for p in new_genfs])
PY
[ $? = 0 ] || { echo "A6L_SEPOLICY_CHECK FAIL (file list)"; exit 1; }
cd $L
# m4 defines = system/sepolicy/build/soong/policy.go (+ board api level from SepolicyM4Defs)
DEFS="-D mls_num_sens=1 -D mls_num_cats=1024 -D target_arch=arm64 -D target_with_asan=false -D target_with_dexpreopt=true -D target_with_native_coverage=false -D target_build_variant=$VAR -D target_full_treble=true -D target_compatible_property=true -D target_treble_sysprop_neverallow=true -D target_enforce_sysprop_owner=true -D target_exclude_build_test=false -D target_requires_insecure_execmem_for_swiftshader=false -D target_enforce_debugfs_restriction=true -D target_recovery=false -D target_restricts_ashmem_usage=${A6L_M4_ASHMEM:-false} -D target_board_api_level=202604"
M4=$L/prebuilts/build-tools/linux-x86/bin/m4
VARS=$L/out/soong/soong.lineage_gsi_a6l.variables
FLAGDEFS=$(python3 -c "
import json,re; v=json.load(open('$VARS')); bf=v.get('BuildFlags',{})
names=sorted(set(re.findall(r'\"(RELEASE_[A-Z0-9_]+)\"',open('$L/system/sepolicy/flagging/Android.bp').read())))
print(' '.join('-D target_flag_%s=%s'%(n,bf.get(n,'')) for n in names))")
ASH=$(python3 -c "import json; print(str(json.load(open('$VARS')).get('RestrictsAshmemUsage', False)).lower())")
DEFS="$DEFS $FLAGDEFS"; DEFS=${DEFS/target_restricts_ashmem_usage=false/target_restricts_ashmem_usage=$ASH}
$M4 --fatal-warnings $DEFS -s $(cat $W/files.txt) > $W/vendor.conf 2> $W/m4.err || { cat $W/m4.err | head; echo "A6L_SEPOLICY_CHECK FAIL (m4)"; exit 1; }
$H/checkpolicy -C -M -c 30 -o $W/vendor.raw.cil $W/vendor.conf > $W/checkpolicy.log 2>&1 || { tail -15 $W/checkpolicy.log; echo "A6L_SEPOLICY_CHECK FAIL (checkpolicy)"; exit 1; }
cp $W/vendor.raw.cil $W/vendor.filtered.cil
$H/build_sepolicy -a $H filter_out -f $S/reqd_policy_mask.cil/android_common/reqd_policy_mask.cil -t $W/vendor.filtered.cil > $W/filter.log 2>&1 || { tail $W/filter.log; echo "A6L_SEPOLICY_CHECK FAIL (filter_out)"; exit 1; }
$H/version_policy -b $S/plat_pub_versioned.cil/android_common/a6l/plat_pub_versioned.cil -t $W/vendor.filtered.cil -n 202604 -o $W/vendor_sepolicy.cil > $W/version.log 2>&1 || { tail $W/version.log; echo "A6L_SEPOLICY_CHECK FAIL (version_policy)"; exit 1; }
$H/build_sepolicy -a $H filter_out -f $S/plat_pub_versioned.cil/android_common/a6l/plat_pub_versioned.cil -t $W/vendor_sepolicy.cil > $W/filter2.log 2>&1 || { tail $W/filter2.log; echo "A6L_SEPOLICY_CHECK FAIL (filter_out plat_pub)"; exit 1; }
P=$S/plat_sepolicy.cil/android_common/plat_sepolicy.cil
$H/secilc -m -M true -G -N -c 30 $P $S/plat_mapping_file/android_common/202604.cil \
  $S/plat_pub_versioned.cil/android_common/a6l/plat_pub_versioned.cil \
  $S/system_ext_sepolicy.cil/android_common/a6l/system_ext_sepolicy.cil $S/system_ext_mapping_file/android_common/a6l/202604.cil \
  $S/product_sepolicy.cil/android_common/a6l/product_sepolicy.cil $S/product_mapping_file/android_common/a6l/202604.cil \
  $W/vendor_sepolicy.cil -o $W/policy.nocheck -f /dev/null > $W/secilc-N.log 2>&1 || { tail -20 $W/secilc-N.log; echo "A6L_SEPOLICY_CHECK FAIL (secilc link)"; exit 1; }
$H/secilc -m -M true -G -c 30 $P $S/plat_mapping_file/android_common/202604.cil \
  $S/plat_pub_versioned.cil/android_common/a6l/plat_pub_versioned.cil \
  $S/system_ext_sepolicy.cil/android_common/a6l/system_ext_sepolicy.cil $S/system_ext_mapping_file/android_common/a6l/202604.cil \
  $S/product_sepolicy.cil/android_common/a6l/product_sepolicy.cil $S/product_mapping_file/android_common/a6l/202604.cil \
  $W/vendor_sepolicy.cil -o $W/policy -f /dev/null > $W/secilc.log 2>&1 || {
  # With VAR=user the vendor half is user-flavoured but the plat half is the r3 (userdebug) plat_sepolicy.cil: the stock tree
  # itself then fails ONE neverallow (mediacodec tcp_socket, userdebug-only plat grant). Anything else is a real failure.
  real=$(grep -A1 "neverallow check failed" $W/secilc.log | grep "(neverallow" | grep -v "(neverallow mediacodec domain (tcp_socket" | wc -l)
  if [ "$VAR" = user ] && [ "$real" = 0 ] && grep -q "(neverallow mediacodec domain (tcp_socket" $W/secilc.log; then
    echo "note: only the known user-vendor/userdebug-plat mismatch (mediacodec tcp_socket) failed; linking with -N for the policy file"
    cp $W/policy.nocheck $W/policy
  else grep -E "neverallow|Error|error" $W/secilc.log | head -40; echo "A6L_SEPOLICY_CHECK FAIL (neverallow)"; exit 1; fi; }
# vendor file_contexts of every dir in the list, validated against the linked policy
: > $W/vendor_file_contexts
for d in $(grep "^$R/device" $W/files.txt | xargs -r -n1 dirname | sort -u); do [ -f $d/file_contexts ] && { echo "# $d"; tr -d '\r' < $d/file_contexts; echo; } >> $W/vendor_file_contexts; done
$H/sefcontext_compile -o $W/fc.bin -p $W/policy $W/vendor_file_contexts > $W/fc.log 2>&1 || { tail $W/fc.log; echo "A6L_SEPOLICY_CHECK FAIL (file_contexts)"; exit 1; }
# property_contexts: every type named must exist in the linked policy
: > $W/vendor_property_contexts
for d in $(grep "^$R/device" $W/files.txt | xargs -r -n1 dirname | sort -u); do [ -f $d/property_contexts ] && tr -d '\r' < $d/property_contexts >> $W/vendor_property_contexts; done
bad=0; for t in $(grep -v '^#' $W/vendor_property_contexts | awk 'NF>=2{split($2,a,":"); print a[3]}' | sort -u); do
  grep -q "(type $t)" $W/vendor_sepolicy.cil $P $S/system_ext_sepolicy.cil/android_common/a6l/system_ext_sepolicy.cil 2>/dev/null || grep -q "(typeattribute ${t}_202604)\|(type ${t})" $S/plat_pub_versioned.cil/android_common/a6l/plat_pub_versioned.cil || { echo "property type not in policy: $t"; bad=1; }; done
[ $bad = 0 ] || { echo "A6L_SEPOLICY_CHECK FAIL (property_contexts)"; exit 1; }
echo "vendor types/rules: $(grep -c '^(type ' $W/vendor_sepolicy.cil) types, $(grep -c '^(allow ' $W/vendor_sepolicy.cil) allow; permissive: $(grep -c typepermissive $W/vendor_sepolicy.cil)"
echo "A6L_SEPOLICY_CHECK PASS variant=$VAR extra=${EXTRA[*]:-none} out=$W"
