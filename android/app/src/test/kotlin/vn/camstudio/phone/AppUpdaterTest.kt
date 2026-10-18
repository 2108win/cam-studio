package vn.camstudio.phone

import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class AppUpdaterTest {
    @Test
    fun comparesDottedNumbers() {
        assertTrue(AppUpdater.isNewer("0.1.4", "0.1.3"))
        assertTrue(AppUpdater.isNewer("0.1.10", "0.1.9"))
        assertTrue(AppUpdater.isNewer("1.0", "0.9.9"))
        assertTrue(AppUpdater.isNewer("0.2", "0.1.9"))
        assertFalse(AppUpdater.isNewer("0.1.3", "0.1.3"))
        assertFalse(AppUpdater.isNewer("0.1.3", "0.1.4"))
        assertFalse(AppUpdater.isNewer("0.1", "0.1.0"))
    }
}
