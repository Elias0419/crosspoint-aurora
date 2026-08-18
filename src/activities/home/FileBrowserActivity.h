#pragma once

#include <memory>
#include <string>
#include <vector>

#include "activities/UiListActivity.h"

class FileBrowserActivity final : public UiListActivity {
 public:
  // Books = standard reader browser; PickFirmware = filter to .bin only and return path via ActivityResult.
  enum class Mode { Books, PickFirmware };

 private:
  // Actions offered by the long-press context menu.
  enum class ContextAction { Rename, Delete };

  // Context menu / deletion / rename
  void showContextMenu(const std::string& entry);
  void confirmAndDelete(const std::string& fullPath, const std::string& displayName);
  void promptRename(const std::string& directory, const std::string& currentName, bool isDir);
  bool removeDirFile(const std::string& fullPath);

  // True when this activity was entered while Confirm was already held; the release that
  // ends that press must not auto-open whatever sits at the selection.
  bool lockNextConfirmRelease = false;
  // Same for a Back carried in from the previous screen: its release is not a jump to root.
  bool lockLongPressBack = false;
  // True once the current Confirm hold has opened the context menu, so it fires once per
  // hold and the eventual release doesn't also open the entry. Reset when Confirm is up.
  bool contextMenuArmed = false;

  Mode mode = Mode::Books;

  // Files state
  std::string basepath = "/";
  std::vector<std::string> files;
  std::unique_ptr<char[]> fileNameBuffer;

  // Per-row render buffers, derived from `files` and rebuilt only when it
  // changes (loadFiles()) rather than on every repaint — buildScreen() used to
  // rebuild a name/extension string and a ListItem per file on every render
  // (cursor move, tap flash, ...), which meant a 500-file directory allocated
  // 500 strings per repaint instead of once per directory load.
  std::vector<std::string> rowNames;
  std::vector<std::string> rowExtensions;
  std::vector<freeink::ui::ListItem> rowItems;
  // getFileName()'s "[folder]" bracket formatting depends on the active
  // theme's showsFileIcons(); tracked so a theme change while this activity is
  // paused underneath (e.g. a Settings screen reached via a picker flow)
  // invalidates the cached rows on return instead of rendering stale ones.
  bool rowsUseFileIcons = false;

  void rebuildRowItems();

  int listCount() const override { return static_cast<int>(files.size()); }
  void buildScreen(UiScreen& screen) override;
  void activateIndex(int index) override;
  void onRowLongPress(int index) override;
  // Long-press BACK goes to root; short Back goes up a directory (home/cancel at
  // root), and Confirm activates on RELEASE (a hold is "delete").
  bool handleCustomInput() override;
  bool handleButtons() override;
  void navigateButtons() override;
  // Header shows the current folder name (battery indicator via GUI.drawHeader);
  // footer labels depend on path depth and picker mode.
  void drawChrome() override;
  void drawFooter() override;
  // Opens the selection: a file goes to the reader, a folder descends. Deleting and
  // renaming live in the long-press context menu instead.
  void activateSelected();

  // Data loading
  void loadFiles();
  size_t findEntry(const std::string& name) const;

 public:
  explicit FileBrowserActivity(GfxRenderer& renderer, MappedInputManager& mappedInput, std::string initialPath = "/",
                               Mode mode = Mode::Books);
  void onEnter() override;
  void onExit() override;
};
