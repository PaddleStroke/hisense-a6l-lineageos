"""Finite, non-retrying COM transfer for the two fixed V12 recovery images.

COM writes are byte-stream writes; an empty COM write is not a USB ZLP.
The caller configures ZLPAwareHost=0, supplies an external process deadline,
checks the exact spare and all pre-write regions, and verifies all readbacks.
"""
from DiagnosticRecoveryProtocolV11 import verify_payload, PROGRAM_XML


def exact_write(device, data):
    if isinstance(data, str):
        data = data.encode('utf-8')
    if not data or not isinstance(data, bytes):
        raise ValueError('Expected one nonempty immutable COM transfer')
    if device.write_timeout != 5:
        raise ValueError('COM write timeout must be five seconds')
    count = device.write(data)
    if type(count) is not int or count != len(data):
        raise IOError(f'Ambiguous/short COM write: {count!r}/{len(data)}; no retry')
    return True


def program(fh, payload, mode, report, save):
    digest = verify_payload(payload, mode)
    if (not fh.cdc.is_serial or fh.cfg.SECTOR_SIZE_IN_BYTES != 512
            or fh.cfg.MemoryName.lower() != 'emmc' or fh.cfg.ZLPAwareHost != 0):
        raise ValueError('Unexpected native serial configuration')
    chunk = fh.cfg.MaxPayloadSizeToTargetInBytes
    if type(chunk) is not int or not 512 <= chunk <= 1048576 or chunk % 512:
        raise ValueError('Unexpected payload transfer size')
    report.update(payload_sha256=digest, bytes_expected=len(payload), bytes_transferred=0,
                  program_attempted=False, raw_mode_pending=False, write_acknowledged=False)
    save()
    original = fh.cdc.write
    fh.cdc.write = lambda data, pktsize=None: exact_write(fh.cdc.device, data)
    try:
        report.update(program_attempted=True, raw_mode_pending=True)
        save()
        response = fh.xmlsend(PROGRAM_XML)
        attributes = response.data if isinstance(response.data, dict) else fh.xml.getresponse(response.data)
        report['initial_response'] = attributes
        save()
        if not response.resp or attributes.get('value') != 'ACK' or attributes.get('rawmode') != 'true':
            raise IOError('Initial raw-mode ACK missing')
        for offset in range(0, len(payload), chunk):
            exact_write(fh.cdc.device, payload[offset:offset + chunk])
            report['bytes_transferred'] += min(chunk, len(payload) - offset)
            save()
        final = fh.xml.getresponse(fh.wait_for_data())
        report['final_response'] = final
        save()
        if final.get('value') != 'ACK' or final.get('rawmode') != 'false':
            raise IOError('Final raw-mode completion ACK missing')
        report.update(raw_mode_pending=False, write_acknowledged=True)
        save()
    except BaseException as error:
        report['transfer_error'] = repr(error)
        save()
        raise
    finally:
        fh.cdc.write = original
