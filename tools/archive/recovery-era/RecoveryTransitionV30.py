"""Allow only known complete recovery images and inert/known bootloader requests.

This validates saved pre-write reads only. It never changes the boot message.
The caller must also check the spare Sahara identity, GPT and target payload.
"""
import hashlib

STOCK = '9688e3dfb349186fb60efc057a618aeaf696b030ad52974932ea83a077b31621'
PREVIOUS = '95b98dd09d5f12703553640ecb0cf6aa109e3ef6ad7bbd98698385d2dfc9d7b6'
RECOVERIES = {STOCK: 'stock', PREVIOUS: 'verified-v29'}
DEVINFO = '7d6a4855f19d498a092ff0ffb44a69e114cd915d4d49acb2640035cf70e854ed'
EMPTY_BCB = 'ad7facb2586fc6e966c004d7d1d16b024f5805ff7cb47c7a85dabd8b48892ca7'
BOOTLOADER_BCB = '8ac9baa0ce2f52dda6debef8f8ffb4fcd8e85d44bf05882dbcb552dd06b46881'


def verify_transition(recovery_digest, bcb, devinfo):
    if recovery_digest not in RECOVERIES:
        raise ValueError('Existing recovery is neither exact stock nor the verified V29 image')
    if len(devinfo) != 4096 or hashlib.sha256(devinfo).hexdigest() != DEVINFO:
        raise ValueError('Spare devinfo differs from the verified unlocked baseline')
    if len(bcb) != 4096:
        raise ValueError('Require the complete 4 KiB boot-control block')
    digest = hashlib.sha256(bcb).hexdigest()
    if digest == EMPTY_BCB and bcb == bytes(4096):
        boot_state = 'empty'
    elif digest == BOOTLOADER_BCB and bcb == b'bootonce-bootloader' + bytes(4096 - 19):
        boot_state = 'known-bootonce-bootloader'
    else:
        raise ValueError('Unknown boot message; no diagnostic replacement permitted')
    return {'previous_recovery': RECOVERIES[recovery_digest],
            'previous_recovery_sha256': recovery_digest,
            'boot_message': boot_state, 'boot_message_sha256': digest,
            'boot_message_preserved': True}
