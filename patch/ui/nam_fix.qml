import QtQuick 2.2

// Instantiated by every encoder-field tile (EncoderParamInfo.qml) while its module is a NAM pedal, on any page (the
// Rig page included): keeps the stock Drive readout in step with the model number, see nam_ui.qml for the why.
Item {
	id: root
	property var t // the tile
	property string pedal: t ? t.mt : ""
	property var drive: pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/Drive") : null
	property double stamp: new Date().getTime()
	property var names: modelList.item ? modelList.item.names : []
	property real raw: drive && drive.translator ? drive.translator.unnormalized : -1
	property int idx: names.length > 0 ? Math.max(0, Math.min(names.length - 1, Math.round(raw) - 1)) : -1
	property string stock: drive && drive.translator ? drive.translator.string : ""
	property string expected: idx >= 0 ? Math.round(raw) + " " + names[idx] : ""

	Loader { id: modelList; source: "file:///tmp/nam_models.qml?" + root.stamp }

	onRawChanged: fixTimer.restart()
	Timer {
		id: fixTimer
		interval: 40; repeat: true; running: true
		onTriggered: {
			if (root.drive && root.drive.translator && root.expected !== "" && root.stock !== root.expected)
				root.drive.translator.unnormalized = root.raw
		}
	}
}
