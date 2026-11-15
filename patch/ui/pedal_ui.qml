import QtQuick 2.2

// Loaded by the patched BlockDetails.qml in place of the (missing) per-pedal editor of the additive pedals; picks the
// page by pedal name. Anything else (the stock Glorious Drive shares this editor slot) shows nothing, as before.
Column {
	id: root
	width: 720
	property var blockPage
	property int navIndex: -1
	property int navigationItemsCount: 0
	function navigationItem(index) { return null }

	property string pedal: blockPage && blockPage.moduleTypeProp ? blockPage.moduleTypeProp.translator.string : ""
	property string kind: pedal.indexOf("NAM") === 0 ? "nam" : (pedal.indexOf("Reverb") === 0 ? "reverb" : "")

	Loader {
		width: 720
		source: root.kind === "" ? "" : Qt.resolvedUrl(root.kind + "_ui.qml")
		onLoaded: item.blockPage = root.blockPage
	}
}
