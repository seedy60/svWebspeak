# Global plugin for svWebspeak: adds the update checker to NVDA's Tools menu.
#
# This is separate from the synth driver on purpose. The driver is only alive
# while svWebspeak is the selected synthesizer, so hosting the updater there
# would mean updates are never checked for by anyone using another synth.

import addonHandler
import globalPluginHandler
import gui
import wx

from ._svGithubUpdater import GitHubReleaseUpdater

addonHandler.initTranslation()

ADDON_NAME = "svWebspeak"
ADDON_LABEL = "svWebspeak"
GITHUB_OWNER = "seedy60"
GITHUB_REPO = "svWebspeak"


class GlobalPlugin(globalPluginHandler.GlobalPlugin):
    def __init__(self):
        super().__init__()
        self._updater = None
        self._checkItem = None
        self._autoItem = None
        # NVDA runs global plugins in every profile, including secure screens,
        # where add-ons cannot be installed and there is no tray menu.
        try:
            self._updater = GitHubReleaseUpdater(
                ADDON_NAME, ADDON_LABEL, GITHUB_OWNER, GITHUB_REPO
            )
        except Exception:
            return
        self._addMenuItems()
        self._updater.start()

    def _addMenuItems(self):
        try:
            toolsMenu = gui.mainFrame.sysTrayIcon.toolsMenu
        except Exception:
            return
        self._checkItem = toolsMenu.Append(
            wx.ID_ANY,
            # Translators: a Tools menu item to check for add-on updates.
            _("Check for svWebspeak &updates..."),
        )
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self._onCheckForUpdates, self._checkItem)
        self._autoItem = toolsMenu.AppendCheckItem(
            wx.ID_ANY,
            # Translators: a Tools menu item toggling automatic update checks.
            _("Check for svWebspeak updates &automatically"),
        )
        self._autoItem.Check(self._updater.autoCheck)
        gui.mainFrame.sysTrayIcon.Bind(wx.EVT_MENU, self._onToggleAutoCheck, self._autoItem)

    def _onCheckForUpdates(self, event):
        if self._updater:
            self._updater.checkNow(True)

    def _onToggleAutoCheck(self, event):
        if self._updater:
            self._updater.autoCheck = event.IsChecked()

    def terminate(self):
        if self._updater:
            self._updater.stop()
        try:
            toolsMenu = gui.mainFrame.sysTrayIcon.toolsMenu
            for item in (self._checkItem, self._autoItem):
                if item:
                    toolsMenu.Remove(item)
        except Exception:
            pass
        super().terminate()
