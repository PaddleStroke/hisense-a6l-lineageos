#!/usr/bin/env python3
"""One hash-pinned diagnostic installation or stock recovery restoration. Not a standalone flashing command.

The caller must establish the spare's exact Sahara identity and matching GPT,
guard Firehose XML, and impose an external process deadline before calling.
This function never retries a failed transfer or resets the phone. A separate
complete readback is required; an acknowledged write is not verification.
"""
import hashlib
import xml.etree.ElementTree as ET

RECOVERY_BYTES = 67108864
STOCK_HASH = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'
DIAGNOSTIC_HASH = '9c092eb41e8c894bff5aea6a1df442582cb4378d782e48d3883aafa1d04bed4e'
TARGETS = {'install-diagnostic': DIAGNOSTIC_HASH, 'restore-stock': STOCK_HASH}
PROGRAM_ATTRIBUTES = {
    'SECTOR_SIZE_IN_BYTES': '512', 'physical_partition_number': '0',
    'start_sector': '917504', 'num_partition_sectors': '131072',
}
PROGRAM_XML = '<?xml version="1.0" ?><data>' + ET.tostring(
    ET.Element('program', PROGRAM_ATTRIBUTES), encoding='unicode') + '</data>'


def check_program_xml(xml):
    root = ET.fromstring(xml)
    if (root.tag != 'data' or root.attrib or len(root) != 1
            or root[0].tag != 'program' or root[0].attrib != PROGRAM_ATTRIBUTES
            or len(root[0]) or (root.text or '').strip()
            or (root[0].text or '').strip() or (root[0].tail or '').strip()):
        raise ValueError('Only the exact full recovery partition program operation is permitted')
    return 'program'


def exact_usb_write(endpoint, payload, timeout=5000):
    """One transfer, exact byte count, no retry after an ambiguous USB outcome."""
    written = endpoint.write(payload, timeout=timeout)
    if type(written) is not int or written != len(payload):
        raise IOError(f'Short USB write: expected {len(payload)}, got {written!r}')
    return True


def verify_payload(payload, mode):
    if mode not in TARGETS:
        raise ValueError('Only the fixed diagnostic-install and stock-restore modes are permitted')
    if not isinstance(payload, bytes) or len(payload) != RECOVERY_BYTES:
        raise ValueError('Recovery payload must be exactly 64 MiB of immutable bytes')
    digest = hashlib.sha256(payload).hexdigest()
    if digest != TARGETS[mode]:
        raise ValueError('Payload does not match the fixed image for this mode')
    return digest


def program_recovery(fh, payload, mode, report, save):
    """Program only the selected hash-pinned recovery; leave subsequent readback to the caller."""
    digest = verify_payload(payload, mode)
    if fh.cfg.SECTOR_SIZE_IN_BYTES != 512 or fh.cfg.MemoryName.lower() != 'emmc':
        raise ValueError('Unexpected storage geometry')
    chunk = fh.cfg.MaxPayloadSizeToTargetInBytes
    if type(chunk) is not int or not 512 <= chunk <= 1048576 or chunk % 512:
        raise ValueError('Unexpected negotiated transfer size')
    check_program_xml(PROGRAM_XML)
    report.update(payload_sha256=digest, bytes_expected=RECOVERY_BYTES,
                  bytes_transferred=0, program_attempted=False, raw_mode_pending=False,
                  write_acknowledged=False, independent_readback_verified=False)
    save()
    original_write = fh.cdc.write

    def strict_write(data, pktsize=None):
        if isinstance(data, str):
            data = data.encode('utf-8')
        # Replace the upstream transport's retry/short-write handling during
        # programming, including the XML request. A failure propagates outward.
        return exact_usb_write(fh.cdc.EP_OUT, data)

    fh.cdc.write = strict_write
    try:
        report['program_attempted'] = True
        report['raw_mode_pending'] = True  # Request outcome can be ambiguous.
        save()
        response = fh.xmlsend(PROGRAM_XML)
        attributes = response.data if isinstance(response.data, dict) else fh.xml.getresponse(response.data)
        report['initial_response'] = attributes
        save()
        if not response.resp or attributes.get('value') != 'ACK' or attributes.get('rawmode') != 'true':
            raise IOError('Programming raw mode was not explicitly acknowledged')
        for offset in range(0, len(payload), chunk):
            data = payload[offset:offset + chunk]
            strict_write(data)
            report['bytes_transferred'] += len(data)
            strict_write(b'')  # Match the established client's ZLP boundary.
            save()
        final = fh.xml.getresponse(fh.wait_for_data())
        report['final_response'] = final
        save()
        if final.get('value') != 'ACK' or final.get('rawmode') != 'false':
            raise IOError('Final programming acknowledgment is missing or unsuccessful')
        report['raw_mode_pending'] = False
        report['write_acknowledged'] = True
        save()
    except BaseException as error:
        report['transfer_error'] = str(error)
        save()
        raise
    finally:
        fh.cdc.write = original_write
