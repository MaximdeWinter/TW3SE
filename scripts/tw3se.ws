// TW3SE (The Witcher 3 Script Extender) - script side.
// Installed as Mods\modTW3SE\content\scripts\local\tw3se.ws next to the extender DLL.

// What the DLC manager knows: tw3se_dlc("dlchotdlc4755", "dlckeirareplica3") - ids as in the .reddlc
exec function tw3se_dlc(optional a : name, optional b : name, optional c : name) {
    var dm : CDLCManager;
    var all : array<name>;
    var i : int;
    var s : string;
    dm = theGame.GetDLCManager();
    dm.GetDLCs(all);
    for (i = 0; i < all.Size(); i += 1) {
        s += NameToString(all[i]) + ";";
    }
    LogChannel('TW3SE', "dlcs|" + IntToString(all.Size()) + "|" + s);
    TW3SEDlcLine(dm, a);
    TW3SEDlcLine(dm, b);
    TW3SEDlcLine(dm, c);
}

function TW3SEDlcLine(dm : CDLCManager, id : name) {
    if (!IsNameValid(id)) {
        return;
    }
    LogChannel('TW3SE', "dlc|" + NameToString(id) + "|available=" + TW3SEYesNo(dm.IsDLCAvailable(id))
        + "|enabled=" + TW3SEYesNo(dm.IsDLCEnabled(id)));
}

function TW3SEYesNo(b : bool) : string {
    if (b) {
        return "y";
    }
    return "n";
}

// Diagnostics: why does loading a save fail after a DLC was mounted at runtime?
// ESessionRestoreResult (engine enum, order from the exe): 0 Success, 1 DataCorrupted, 2 DLCRequired,
// 3 MissingContent, 4 InternalError, 5 NoGameDefinition, 6 WrongGameVersion
@wrapMethod(CR4GuiManager)
function OnLoadingFailed(sres : ESessionRestoreResult, missingContent : array<name>) : void {
    var i : int;
    var s : string;
    for (i = 0; i < missingContent.Size(); i += 1) {
        s += NameToString(missingContent[i]) + ";";
    }
    LogChannel('TW3SE', "loading failed|sres=" + IntToString((int)sres) + "|missing=" + s);
    wrappedMethod(sres, missingContent);
}
