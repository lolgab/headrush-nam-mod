import QtQuick 2.2

// Overlay on the signal-chain block (Rig page) of the additive pedals, drawn on top of the stock stomp-box picture
// (ActiveStomp.qml loads it for NAM and Reverb pedals; the picture, the name, the LED and the footswitch stay the stock
// ones, the Reverb's picture is recoloured blue there): three small knobs that turn with the live values, and under the
// pedal name the model (NAM) or the algorithm (Reverb).
Item {
	id: root
	anchors.fill: parent // the block cell, 183 x 285
	property var m // the SignalChainModule

	property string pedal: m ? m.moduleName : ""
	property bool isNam: pedal.indexOf("NAM") === 0
	property bool isReverb: pedal.indexOf("Reverb") === 0
	function prop(n) { return pedal !== "" ? Evil.getProperty("/Engine/Patch/" + pedal + "/" + n) : null }
	function norm(pr) { return pr && pr.translator ? Math.max(0, Math.min(1, pr.translator.value)) : 0.5 }

	property var driveP: prop("Drive")
	property var kv: [norm(driveP), norm(prop("Level")), norm(prop("Tone"))]

	property var gainP: prop("Gain")
	property var hicutP: prop("Hi-Cut")
	property int algo: gainP && hicutP && gainP.translator && hicutP.translator
		? (hicutP.translator.index === 0 ? (gainP.translator.index === 1 ? 3 : 2) : (gainP.translator.index === 1 ? 1 : 0)) : 0

	property double stamp: new Date().getTime()
	Loader { id: modelList; source: root.isNam ? "file:///tmp/nam_models.qml?" + root.stamp : "" }
	property var names: modelList.item ? modelList.item.names : []
	property int modelIdx: driveP && driveP.translator && names.length > 0
		? Math.max(0, Math.min(names.length - 1, Math.round(driveP.translator.unnormalized) - 1)) : -1
	property string caption: isReverb ? ["PLATE", "HALL", "ROOM", "SPRING"][algo] : (modelIdx >= 0 ? (modelIdx + 1) + " " + names[modelIdx] : "")

	Repeater {
		model: 3
		Item {
			x: 91 + (index - 1) * 44 - 15; y: 36; width: 30; height: 30
			Rectangle { anchors.fill: parent; radius: 15; color: "#17181a"; border.width: 2; border.color: "#0b0b0c" }
			Item {
				anchors.fill: parent
				rotation: -135 + 270 * root.kv[index]
				Rectangle { x: 13; y: 3; width: 4; height: 11; radius: 2; color: "#f2f2f2" }
			}
		}
	}

	Text {
		x: 17; y: 130; width: 148; horizontalAlignment: Text.AlignHCenter; elide: Text.ElideRight
		text: root.caption; color: "#202124"; font.pixelSize: 14; font.bold: true
	}
}
