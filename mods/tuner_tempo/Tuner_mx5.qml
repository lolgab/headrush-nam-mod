import "../PageController"

EvilPage { id: tuner
	pageHeader.titleText: "Tuner"
	pageHeader.hasPageMenu: false
	pageHeader.hasPageButton: false
	pageHeader.offsetX: 12

	property real midOpac: 0
	function setMidOpac( opac ) { midOpac = opac; }

	FootSwitchInfo {  z: 1; x: 10; y: 348; height: 80; width: 780; numInfo: 3; fieldwidth: 254; fieldHeight: 74; fontSize: 40; fontSizeSmall: 22 } 

	property QObjProperty textProp: Evil.getProperty("/Engine/FFTCtrl/TunerString")
	property bool hasTuning: textProp.translator.string.length > 0

	property QObjProperty centProp: Evil.getProperty("/Engine/FFTCtrl/TunerCents")
	property bool isGood: centProp.translator.unnormalized > -6.0 && centProp.translator.unnormalized < 6.0
	property bool isFlat: centProp.translator.unnormalized <= -6.0
	property bool isSharp: centProp.translator.unnormalized >= 6.0

	MouseArea { onClicked: { tuner.cppPage.focusMain(); mouse.accepted = false; }
	anchors.fill: parent
	propagateComposedEvents: true

	Rectangle { color: "#121314"; x: 0; y:0; width: parent.width; height: parent.height
	DevAccessorButton { z: 10; x: 0; y: 0; width: 30; height: 30 }
	ElevenText { x: 12;   y: 102; 	font.pixelSize: 21; font.letterSpacing: 0.7; regularNotMedium: false; color: "#FCD73F";  text: "-50" }
	ElevenText { x: 748; y: 102; 	font.pixelSize: 21; font.letterSpacing: 0.7; regularNotMedium: false; color: "#FCD73F";  text: "+50" }
	
	TunerMeterV3 { id: tunerMeter; hasTuning: tuner.hasTuning; x: 15; y: 134; height: 106; width: 778; prop: Evil.getProperty("/Engine/FFTCtrl/TunerCents")  }
	
	TunerText  { textSize: 100; 
	height: 150; width: 200; 
	anchors.bottom: tunerMeter.top
	anchors.bottomMargin: 4
	anchors.horizontalCenter: tunerMeter.horizontalCenter
	anchors.horizontalCenterOffset: -5
	prop: Evil.getProperty("/Engine/FFTCtrl/TunerString") }

	Image { z:99; anchors.bottom: tunerMeter.top; anchors.horizontalCenter: parent.horizontalCenter; width: 29 
	source: "qrc:/img/TunerTriangle.png"  
	fillMode: Image.PreserveAspectFit }

	Image { z:100; anchors.bottom: tunerMeter.top; anchors.horizontalCenter: parent.horizontalCenter; width: 29
	source: "qrc:/img/TunerTriangleGreen.png"; 
	fillMode: Image.PreserveAspectFit
	opacity: tuner.midOpac  
	visible: tuner.hasTuning }

	Image { z:99; anchors.bottomMargin: 7; anchors.bottom: tunerMeter.top; anchors.horizontalCenter: parent.horizontalCenter; width: 189 
	source: "qrc:/img/TunerDoubleArrowLeft.png"  
	fillMode: Image.PreserveAspectFit
	visible: (tuner.isGood || tuner.isFlat) && tuner.hasTuning 	}

	Image {	z:99; anchors.bottomMargin:7; anchors.bottom: tunerMeter.top; anchors.horizontalCenter: parent.horizontalCenter; width: 189
	source: "qrc:/img/TunerDoubleArrowRight.png"  
	fillMode: Image.PreserveAspectFit
	visible: (tuner.isGood || tuner.isSharp) && tuner.hasTuning }


	TunerIncDec  { width: 295; height: 60; radius: 4; inBord: 6; bord: 5; bordWidth: 3; x: 11; y: 263; textSize_s: 25; textSize_l: 28; headline: "Reference"; grid: 1/70;
	prop: Evil.getProperty("/Engine/FFTCtrl/TunerRef")
	onClicked: tuner.cppPage.focusTunerRef()
	hasFocus: tuner.cppPage.tunerRefFocused 
	}
MouseArea{x:469;y:263;width:316;height:60;property var t:Evil.getP("/Engine/TempoCtrl/Tempo");onClicked:t.translator.unnormalized+=mouseX<width/2?-1:1;TunerBPMInfo{anchors.fill:parent;relFieldWidth:0.60;radius:4;textSize_s:25;textSize_l:28;inBord:6;headline:"Tempo";prop:parent.t}}                                                             
	TunerMuteButton { width: 142; height: 60; bord: 2; radius: 4; textSize: 31; x: 316; y: 263 }
	}
	}
}
