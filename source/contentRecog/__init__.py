#contentRecog/__init__.py
#A part of NonVisual Desktop Access (NVDA)
#Copyright (C) 2017 NV Access Limited
#This file is covered by the GNU General Public License.
#See the file COPYING for more details.

"""Framework for recognition of content; OCR, image recognition, etc.
When authors don't provide sufficient information for a screen reader user to determine the content of something,
various tools can be used to attempt to recognize the content from an image .
Some examples are optical character recognition (OCR) to recognize text in an image
and the Microsoft Cognitive Services Computer Vision and Google Cloud Vision APIs to describe images.
Recognizers take an image and produce text.
They are implemented using the L{ContentRecognizer} class
and registered by calling the L{registerRecognizer} function.
"""

from collections import namedtuple
import textInfos.offsets
import api
import ui
import screenBitmap
import NVDAObjects.window
import controlTypes
import browseMode
import cursorManager
import eventHandler

#: Registered recognizers.
recognizers = []
#: The selected recognizer for subsequent recognition.
selectedRecognizer = None
#: Whether this framework has been initialized.
_isInitialized = False

class ContentRecognizer(object):
	"""Implementation of a content recognizer.
	"""

	def recognize(self, pixels, left, top, width, height, onResult):
		"""Asynchronously recognize content from an image.
		This method should not block.
		Only one recognition can be performed at a time.
		@param pixels: The pixels of the image as a two dimensional array of RGBQUADs.
			For example, to get the red value for the coordinate (1, 2):
			pixels[2][1].rgbRed
			This can be treated as raw bytes in BGRA8 format;
			i.e. four bytes per pixel in the order blue, green, red, alpha.
			However, the alpha channel should be ignored.
		@type pixels: Two dimensional array (y then x) of L{winGDI.RGBQUAD}
		@param left: The x screen coordinate of the upper-left corner of the image.
			This should be added to any x coordinates returned to NVDA.
		@type left: int
		@param top: The y screen coordinate of the upper-left corner of the image.
			This should be added to any y coordinates returned to NVDA.
		@type top: int
		@param width: The width of the image in pixels.
		@type width: int
		@param height: The height of the image in pixels.
		@type height: int
		@param onResult: A callable which takes a L{RecognitionResult} (or an exception on failure) as its only argument.
		@type onResult: callable
		"""
		raise NotImplementedError

	def cancel(self):
		"""Cancel the recognition in progress (if any).
		"""
		raise NotImplementedError

class RecognitionResult(object):
	"""Provides access to the result of recognition by a recognizer.
	The result is textual, but to facilitate navigation by word, line, etc.
	and to allow for retrieval of screen coordinates within the text,
	L{TextInfo} objects are used.
	Callers use the L{makeTextInfo} method to create a L{TextInfo}.
	Most implementers should use one of the subclasses provided in this module.
	"""

	def makeTextInfo(self, obj, position):
		"""Make a TextInfo within the recognition result text at the requested position.
		@param obj: The object to return for the C{obj} property of the TextInfo.
			The TextInfo itself doesn't use this, but NVDA requires it to set the review object, etc.
		@param position: The requested position; one of the C{textInfos.POSITION_*} constants.
		@return: The TextInfo at the requested position in the result.
		@rtype: L{textInfos.TextInfo}
		"""
		raise NotImplementedError

# Used by LinesWordsResult.
LwrWord = namedtuple("LwrWord", ("offset", "left", "top"))

class LinesWordsResult(RecognitionResult):
	"""A L{RecognizerResult} which can create TextInfos based on a simple lines/words data structure.
	The data structure is a list of lines, wherein each line is a list of words,
	wherein each word is a dict containing the keys x, y, width, height and text.
	Several OCR engines produce output in a format which can be easily converted to this.
	"""

	def __init__(self, data, left, top):
		"""Constructor.
		@param data: The lines/words data structure. For example:
			[
				[
					{"x": 106, "y": 91, "width": 11, "height": 9, "text": "Word1"},
					{"x": 117, "y": 91, "width": 11, "height": 9, "text": "Word2"}
				],
				[
					{"x": 106, "y": 105, "width": 11, "height": 9, "text": "Word3"},
					{"x": 117, "y": 105, "width": 11, "height": 9, "text": "Word4"}
				]
			]
		@type data: list of lists of dicts
		@param left: The x screen coordinate of the upper-left corner of the image.
			This should be added to any x coordinates returned to NVDA.
		@type left: int
		@param top: The y screen coordinate of the upper-left corner of the image.
			This should be added to any y coordinates returned to NVDA.
		@type top: int
		"""
		self.data = data
		self.left = left
		self.top = top
		self._textList = []
		self.textLen = 0
		#: End offsets for each line.
		self.lines = []
		#: Start offsets and screen coordinates for each word.
		self.words = []
		self._parseData()
		self.text = "".join(self._textList)

	def _parseData(self):
		for line in self.data:
			firstWordOfLine = True
			for word in line:
				if firstWordOfLine:
					firstWordOfLine = False
				else:
					# Separate with a space.
					self._textList.append(" ")
					self.textLen += 1
				self.words.append(LwrWord(self.textLen,
					self.left + word["x"],
					self.top + word["y"]))
				text = word["text"]
				self._textList.append(text)
				self.textLen += len(text)
			self.lines.append(self.textLen)

	def makeTextInfo(self, obj, position):
		return LwrTextInfo(obj, position, self)

class LwrTextInfo(textInfos.offsets.OffsetsTextInfo):
	"""TextInfo used by L{LinesWordsResult}.
	This should only be instantiated by L{LinesWordsResult}.
	"""

	def __init__(self, obj, position, result):
		self.result = result
		super(LwrTextInfo, self).__init__(obj, position)

	def copy(self):
		return self.__class__(self.obj, self.bookmark, self.result)

	def _getTextRange(self, start, end):
		return self.result.text[start:end]

	def _getStoryLength(self):
		return self.result.textLen

	def _getLineOffsets(self, offset):
		start = 0
		for end in self.result.lines:
			if end > offset:
				return (start, end)
			start = end
		# offset is too big. Fail gracefully by returning the last line.
		return (start, self.result.textLen)

	def _getWordOffsets(self, offset):
		start = 0
		for word in self.result.words:
			if word.offset > offset:
				return (start, word.offset)
			start = word.offset
		# offset is in the last word (or offset is too big).
		return (start, self.result.textLen)

	def _getPointFromOffset(self, offset):
		word = None
		for nextWord in self.result.words:
			if nextWord.offset > offset:
				# Stop! We need the word before this.
				break
			word = nextWord
		return textInfos.Point(word.left, word.top)

def registerRecognizer(recognizer):
	"""Register a content recognizer.
	@param recognizer: The recognizer to register.
	@type recognizer: L{ContentRecognizer}
	"""
	recognizers.append(recognizer)

def ensureInit():
	global _isInitialized, selectedRecognizer
	if _isInitialized:
		return
	# Register and select builtin recognizer.
	from . import uwpOcr
	registerRecognizer(uwpOcr.UwpOcr())
	selectedRecognizer = recognizers[0]
	_isInitialized = True

class RecogResultNVDAObject(cursorManager.CursorManager, NVDAObjects.window.Window):
	"""Fake NVDAObject used to present a recognition result in a cursor manager.
	This allows the user to read the result with cursor keys, etc.
	Pressing enter will activate (e.g. click) the text at the cursor.
	Pressing escape dismisses the recognition result.
	"""

	role = controlTypes.ROLE_DOCUMENT
	# Translators: The title of the document used to present the result of content recognition.
	name = _("Result")
	treeInterceptor = None

	def __init__(self, result=None, obj=None):
		self.parent = parent = api.getFocusObject()
		self.result = result
		self._selection = self.makeTextInfo(textInfos.POSITION_FIRST)
		super(RecogResultNVDAObject, self).__init__(windowHandle=parent.windowHandle)

	def makeTextInfo(self, position):
		# Maintain our own fake selection/caret.
		if position == textInfos.POSITION_SELECTION:
			ti = self._selection.copy()
		elif position == textInfos.POSITION_CARET:
			ti = self._selection.copy()
			ti.collapse()
		else:
			ti = self.result.makeTextInfo(self, position)
		return self._patchTextInfo(ti)

	def _patchTextInfo(self, info):
		# Patch TextInfos so that updateSelection/Caret updates our fake selection.
		info.updateCaret = lambda: self._setSelection(info, True)
		info.updateSelection = lambda: self._setSelection(info, False)
		# Ensure any copies get patched too.
		oldCopy = info.copy
		info.copy = lambda: self._patchTextInfo(oldCopy())
		return info

	def _setSelection(self, textInfo, collapse):
		self._selection = textInfo.copy()
		if collapse:
			self._selection.collapse()

	def setFocus(self):
		ti = self.parent.treeInterceptor
		if isinstance(ti, browseMode.BrowseModeDocumentTreeInterceptor):
			# Normally, when entering browse mode from a descendant (e.g. dialog),
			# we want the cursor to move to the focus (#3145).
			# However, we don't want this for math, as math isn't focusable.
			ti._enteringFromOutside = True
		eventHandler.executeEvent("gainFocus", self)

	def script_activatePosition(self, gesture):
		self._selection.activate()
	# Translators: Describes a command.
	script_activatePosition.__doc__ = _("Activates the text at the cursor if possible")

	def script_exit(self, gesture):
		eventHandler.executeEvent("gainFocus", self.parent)
	# Translators: Describes a command.
	script_exit.__doc__ = _("Dismiss the recognition result")

	__gestures = {
		"kb:enter": "activatePosition",
		"kb:escape": "exit",
	}

#: Keeps track of the recognition in progress, if any.
_activeRecog = None
def recognizeNavigatorObject():
	"""User interface function to recognize content in the navigator object.
	This should be called from a script or in response to a GUI action.
	"""
	global _activeRecog
	if not selectedRecognizer:
		# Translators: Reported when no recognizers are available.
		ui.message(_("No content recognizers available"))
		return
	if _activeRecog:
		_activeRecog.cancel()
	# Translators: Reporting when content recognition (e.g. OCR) begins.
	ui.message(_("Recognizing"))
	nav = api.getNavigatorObject()
	left, top, width, height = nav.location
	sb = screenBitmap.ScreenBitmap(width, height)
	pixels = sb.captureImage(left, top, width, height)
	_activeRecog = selectedRecognizer
	_activeRecog.recognize(pixels, left, top, width, height, _recogOnResult)

def _recogOnResult(result):
	global _activeRecog
	_activeRecog = None
	if isinstance(result, Exception):
		# Translators: Reported when recognition (e.g. OCR) fails.
		ui.message(_("Recognition failed"))
		return
	resObj = RecogResultNVDAObject(result=result)
	resObj.setFocus()
