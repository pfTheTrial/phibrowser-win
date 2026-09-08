// The box-side paths every rig used to spell out by hand.
//
// Seventeen files carried `C:\Users\jdxin\…` literally (lane 6 §2 item 6);
// they now ask here. Override with PHI_BOX_HOME / PHI_USER_DATA_DIR in the
// environment so a second box, or a second profile, needs no edit.
//
// PHI_USER_DATA_DIR is the SAME variable phi-test.mjs reads for pipe
// discovery, so a suite and its library never disagree about which profile
// is under test.

export const BOX_HOME = process.env.PHI_BOX_HOME || 'C:\\Users\\jdxin'

/** The user-data-dir the client under test is running on. Must agree with
 *  the one phi-test.mjs discovers the pipe in (`--user-data-dir`). */
export const USER_DATA_DIR =
  process.env.PHI_USER_DATA_DIR || 'C:\\phi-fx-test'

/** A path under the box home: boxPath('drag.txt') -> C:\Users\jdxin\drag.txt */
export const boxPath = (...parts) => [BOX_HOME, ...parts].join('\\')

/** A path inside the profile under test. */
export const profilePath = (...parts) => [USER_DATA_DIR, ...parts].join('\\')

/** Where the suites and this library live on the box. */
export const TEST_DIR = process.env.PHI_TEST_DIR || boxPath('phitest')
