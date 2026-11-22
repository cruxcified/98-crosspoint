# Fork copy of the FreeInk SDK BleKeyboardHost

Copied from `freeink-sdk/libs/network/BleKeyboardHost` at SDK revision
6a208c0. The fork changes it to read NKRO keyboards, which send their keys
as a bitmap (one bit per key) instead of six key codes. The 8BitDo keyboard
does this. `platformio.ini` points at this copy instead of the SDK.

Changes: `bitmapKeysOf` and the bitmap branch in `onReportIngest`, and
`kMaxInputReportChars` raised from 4 to 8. Run the tests with
`sh test/host/run.sh`.
