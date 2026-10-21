# VGRPresenter 0.0.9

## Improvements

- **A search in The Table or Scripture now ends when you take a result.** Press **Enter** or **double-click** a row and that sermon or passage opens, the results go away, and the box returns to the passage you are now on. A single click only selects the row.
- **The result list shows its verses again.** Hovering down a list of results used to lose the preview once the pointer moved between rows; the verses on the right now follow the row you are on, and the keyboard walk was already doing this.
- **Esc really takes you back.** Whatever you were reading — the passage, the paragraphs picked, the slide, the row under your eye — is restored, from anywhere in the pane, not only while the search box still has the caret.
- **The arrows in a search stay where they belong.** Left/Right move between the **results** and **the verses beside them**; they no longer park the selector on the years or sermons columns. Clicking a year or a sermon still narrows the results to it, and all three panes now update together.
- **The selector travels exactly three panes when browsing** — years, sermons, paragraphs. It no longer disappears into the template column on the far right (that column has no selector and no accent border now).
- **Up/Down in the verses pane reads paragraph by paragraph**, with the paragraph you are on visibly marked.
- **‹ › step the pick and move the monitor preview again**, in Scripture as well as The Table — from the on-screen arrows and from the Left/Right keys when you are browsing.
- **The Shows tab's search works.** Typing in the dock's search box now filters Shows, Media, Overlays and Templates as you type; before, the typed query was never pushed to the tab.
- **Esc closes, too.** With nothing being typed and nothing selected, **Esc** on the Preview page closes the show you are looking at — exactly what the "…" menu's **Close** does — and the home splash comes back. In the editor it takes you back to Preview, and if the "…" menu is open it just puts the menu away.

## What's new

- **Two search stops, not four.** While a search answer is up the selector has exactly two places to be: the result list and the verses of the highlighted result. The years and sermons are narrowed by clicking a row.
- **Hover and the keyboard share one cursor.** The row under the pointer is the row Up/Down continues from, so the highlight and the walk can never point at different results.
- **Search results open like every other card:** single click selects and previews, double-click (or Enter) opens.
- **Ending a search clears the query**, so the box falls back to the seed line of the passage that was just opened — ready for the paragraph or verse digits.
- **Esc is the keyboard half of Close.** The centre page's “…” menu owns what closing means and Esc fires the same signal, so the two can never drift; a layer that already answers Esc (a dialog, a picker, a label being typed into, a canvas selection) keeps it first.
- The “…” menu's **Edit show** / **Close** rows are white now — they are the actions, not another line of the title.

## Fixes

- Hovering down a result list no longer leaves the verses pane empty when the pointer returns to a row.
- A click on a year or sermon during a search now updates the third pane as well, instead of leaving the previous answer on screen.
- The selector no longer stops on the template column, and that column no longer paints a selection edge.
- Enter on a walked result no longer leaves the old query standing in the box as if the search were still running.
- Esc restores the pre-search place even when the search box has lost the caret.
- The Projects panel takes its colours from the app theme instead of FreeShow's own dark palette, so its names follow Appearance — Light included — rather than a fixed off-white on a panel that stayed dark.
- Shows/Media/Overlays/Templates search: the debounced query was dropped before it reached the tab, so typing filtered nothing. It now lands, and the pane filters as you type.
- The floating ‹ › buttons and the Left/Right keys step the pick and stage the monitor preview again (they had been repurposed to move a selector that no longer exists in that spot).

## Windows downloads

- **VGRPresenter-Setup-0.0.9.exe** (recommended installer).
- **VGRPresenter-win64.zip** (portable version; unzip it and open the app).

Requires Windows 10 or newer.
