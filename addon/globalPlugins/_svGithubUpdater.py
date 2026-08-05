# GitHub release updater for svWebspeak.
#
# Modelled on the updater in Andre Louis's Prose 2000 add-on
# (https://github.com/OnjLouis/prose2000), with one substantive difference:
# svWebspeak's releases attach a .zip holding the .nvda-addon alongside
# sv_license.reg, because the registration file has to reach the user too.
# A release asset is therefore accepted in either form - a bare .nvda-addon,
# or a .zip that contains one.

import json
import os
import threading
import time
import urllib.request
import zipfile

import addonHandler
import config
import globalVars
import gui
import wx
from core import callLater
from logHandler import log

try:
    addonHandler.initTranslation()
except Exception:
    pass

try:
    from gui.addonGui import promptUserForRestart
except ImportError:
    promptUserForRestart = None

# Moved out of gui in later NVDA releases; the add-on supports back to 2019.3.
try:
    from systemUtils import ExecAndPump
except ImportError:
    from gui import ExecAndPump


CHECK_INTERVAL_MS = 86400 * 1000
RETRY_INTERVAL_MS = 600 * 1000
DOWNLOAD_BLOCK_SIZE = 8192
USER_AGENT = "svWebspeak NVDA add-on updater"
# Release notes land in a message box that is read aloud, so keep them short
# enough to be listenable rather than dumping an entire changelog.
MAX_NOTES_CHARS = 700


def _parseVersion(version):
    """Split a version into a comparable tuple.

    svWebspeak versions are a single increasing integer (2520, 2521, ...) and
    release tags match them (v2520). Dotted versions still parse, but note that
    the two schemes are not meaningfully comparable with each other, so a
    release must not switch schemes: 2520 would outrank any 1.x forever.
    """
    try:
        return tuple(int(part) for part in str(version).strip().lstrip("vV").split("."))
    except Exception:
        return (0,)


def _downloadFile(url, destination, update=None):
    request = urllib.request.Request(url, headers={"User-Agent": USER_AGENT})
    with urllib.request.urlopen(request, timeout=120) as remote, open(destination, "wb") as local:
        size = int(remote.headers.get("content-length") or 0)
        read = 0
        while True:
            block = remote.read(DOWNLOAD_BLOCK_SIZE)
            if not block:
                break
            local.write(block)
            read += len(block)
            if update and size and update(int(read / size * 100)):
                return False
    return True


def _extractAddonFromZip(zipPath, targetDir):
    """Pull the .nvda-addon out of a release zip.

    Only the member's base name is used when writing it out, so a crafted
    archive cannot write outside targetDir.
    """
    with zipfile.ZipFile(zipPath) as archive:
        member = next(
            (n for n in archive.namelist() if n.lower().endswith(".nvda-addon")),
            None,
        )
        if not member:
            raise RuntimeError("The downloaded archive contains no .nvda-addon package.")
        destination = os.path.join(targetDir, os.path.basename(member))
        with archive.open(member) as source, open(destination, "wb") as local:
            while True:
                block = source.read(DOWNLOAD_BLOCK_SIZE)
                if not block:
                    break
                local.write(block)
    return destination


class GitHubReleaseUpdater:
    def __init__(self, addonName, addonLabel, owner, repo):
        self.addonName = addonName
        self.addonLabel = addonLabel
        self.apiUrl = "https://api.github.com/repos/%s/%s/releases/latest" % (owner, repo)
        self.updatesDir = os.path.join(globalVars.appArgs.configPath, "updates")
        self.stateFile = os.path.join(globalVars.appArgs.configPath, "%sGithubUpdate.json" % addonName)
        self.timer = None
        self.busy = False
        self.stopped = False
        self.hadError = False
        self.state = self._loadState()

    # ------------------------------------------------------------- state

    def _loadState(self):
        try:
            with open(self.stateFile, "r", encoding="utf-8") as source:
                state = json.load(source)
        except Exception:
            state = {}
        state.setdefault("lastCheck", 0)
        state.setdefault("autoCheck", True)
        return state

    def _saveState(self):
        try:
            with open(self.stateFile, "w", encoding="utf-8") as destination:
                json.dump(self.state, destination)
        except Exception:
            log.debugWarning("Could not save updater state for %s" % self.addonName, exc_info=True)

    @property
    def autoCheck(self):
        return bool(self.state.get("autoCheck", True))

    @autoCheck.setter
    def autoCheck(self, value):
        self.state["autoCheck"] = bool(value)
        self._saveState()
        if value:
            self._scheduleNext()
        else:
            self._cancelTimer()

    # ----------------------------------------------------------- control

    def start(self):
        # The Windows Store build cannot install add-ons from outside itself.
        if getattr(config, "isAppX", False):
            return
        self.stopped = False
        if self.autoCheck:
            self._scheduleNext()

    def stop(self):
        self.stopped = True
        self._cancelTimer()

    def _cancelTimer(self):
        try:
            if self.timer and self.timer.IsRunning():
                self.timer.Stop()
        except Exception:
            pass
        self.timer = None

    def _scheduleNext(self):
        if self.stopped or not self.autoCheck:
            return
        self._cancelTimer()
        interval = RETRY_INTERVAL_MS if self.hadError else CHECK_INTERVAL_MS
        delay = int(interval - (time.time() * 1000 - self.state.get("lastCheck", 0)))
        # Never fire immediately: NVDA is still starting up when start() runs.
        self.timer = callLater(max(10000, delay), self.checkNow, False)

    def checkNow(self, fromGui=True):
        if self.busy:
            if fromGui:
                gui.messageBox(
                    # Translators: reported when a check is already running.
                    _("An update check is already in progress."),
                    # Translators: title of the svWebspeak update dialogs.
                    _("svWebspeak updates"),
                    wx.OK,
                    gui.mainFrame,
                )
            return
        self.busy = True
        threading.Thread(
            target=self._checkWorker,
            args=(fromGui,),
            name="svWebspeak update check",
            daemon=True,
        ).start()

    # ------------------------------------------------------------- check

    def _currentAddon(self):
        for addon in addonHandler.getAvailableAddons():
            if addon.name == self.addonName:
                return addon
        return None

    def _getUpdateInfo(self):
        request = urllib.request.Request(self.apiUrl, headers={"User-Agent": USER_AGENT})
        with urllib.request.urlopen(request, timeout=20) as response:
            data = json.loads(response.read().decode("utf-8"))
        assets = data.get("assets", [])
        # A directly attached package is preferred; a zip is unwrapped later.
        asset = next(
            (i for i in assets if str(i.get("name", "")).lower().endswith(".nvda-addon")),
            None,
        )
        if not asset:
            asset = next(
                (i for i in assets if str(i.get("name", "")).lower().endswith(".zip")),
                None,
            )
        if not asset:
            raise RuntimeError("The latest release has no NVDA add-on package.")
        notes = data.get("body") or _("No release notes are available.")
        if len(notes) > MAX_NOTES_CHARS:
            notes = notes[:MAX_NOTES_CHARS].rstrip() + "..."
        return {
            "version": str(data.get("tag_name", "")).lstrip("vV"),
            "name": asset["name"],
            "url": asset["browser_download_url"],
            "notes": notes,
        }

    def _checkWorker(self, fromGui):
        try:
            info = self._getUpdateInfo()
            error = None
        except Exception as caught:
            info = None
            error = caught
        wx.CallAfter(self._handleCheck, fromGui, info, error)

    def _handleCheck(self, fromGui, info, error):
        self.busy = False
        if self.stopped:
            return
        current = self._currentAddon()
        if error or current is None:
            self.hadError = True
            log.debugWarning(
                "Could not check GitHub updates for %s: %s"
                % (self.addonName, error or "add-on not found")
            )
            if fromGui:
                gui.messageBox(
                    # Translators: reported when the update check could not run.
                    _("Unable to check for updates right now."),
                    # Translators: title of the failed update check dialog.
                    _("Update check failed"),
                    wx.OK | wx.ICON_ERROR,
                    gui.mainFrame,
                )
            self._scheduleNext()
            return
        self.hadError = False
        self.state["lastCheck"] = time.time() * 1000
        self._saveState()
        if _parseVersion(info["version"]) <= _parseVersion(current.version):
            if fromGui:
                gui.messageBox(
                    # Translators: reported when the installed version is current.
                    _("There are no updates available for %s.") % self.addonLabel,
                    # Translators: title of the no-updates dialog.
                    _("No updates available"),
                    wx.OK | wx.ICON_INFORMATION,
                    gui.mainFrame,
                )
            self._scheduleNext()
            return
        # The version and the question come first so the essential part is heard
        # before any release notes.
        message = _(
            "A new version of {addon} is available: {version}.\n\n"
            "Do you want to download and install it now?\n\n{notes}"
        ).format(addon=self.addonLabel, version=info["version"], notes=info["notes"])
        answer = gui.messageBox(
            message,
            # Translators: title of the update available dialog.
            _("Update available"),
            wx.YES | wx.NO | wx.ICON_INFORMATION,
            gui.mainFrame,
        )
        if answer == wx.YES:
            self._downloadAndInstall(info)
        self._scheduleNext()

    # ----------------------------------------------------------- install

    def _downloadAndInstall(self, info):
        os.makedirs(self.updatesDir, exist_ok=True)
        destination = os.path.join(self.updatesDir, info["name"])
        extracted = None
        gui.mainFrame.prePopup()
        dialog = wx.ProgressDialog(
            # Translators: title of the download progress dialog.
            _("Downloading svWebspeak update"),
            # Translators: message shown while the update downloads.
            _("Downloading update"),
            style=wx.PD_CAN_ABORT | wx.PD_ELAPSED_TIME | wx.PD_REMAINING_TIME | wx.PD_AUTO_HIDE,
            parent=gui.mainFrame,
        )
        try:
            def update(value):
                return not dialog.Update(value)[0]

            result = ExecAndPump(_downloadFile, info["url"], destination, update)
            if result.funcRes is False:
                return  # cancelled by the user
            package = destination
            if destination.lower().endswith(".zip"):
                extracted = _extractAddonFromZip(destination, self.updatesDir)
                package = extracted
            bundle = addonHandler.AddonBundle(package)
            previous = self._currentAddon()
            installResult = ExecAndPump(addonHandler.installAddonBundle, bundle)
            installed = installResult.funcRes
            if getattr(bundle, "_installExceptions", None):
                raise RuntimeError("NVDA rejected the downloaded add-on.")
            if previous:
                previous.requestRemove()
            if installed and hasattr(installed, "_cleanupAddonImports"):
                installed._cleanupAddonImports()
            if promptUserForRestart:
                promptUserForRestart()
        except Exception:
            log.error("Could not install the svWebspeak update", exc_info=True)
            gui.messageBox(
                # Translators: reported when installing the update failed.
                _("Failed to install the update."),
                # Translators: title of the failed install dialog.
                _("Update failed"),
                wx.OK | wx.ICON_ERROR,
                gui.mainFrame,
            )
        finally:
            dialog.Destroy()
            gui.mainFrame.postPopup()
            for path in (destination, extracted):
                if not path:
                    continue
                try:
                    os.remove(path)
                except OSError:
                    pass
