class BookMenu extends MovieClip
{
   // ---- Original properties ----
   var BookPages;
   var PageInfoA;
   var RefTextFieldTextFormat;
   var ReferenceTextField;
   var ReferenceTextInstance;
   var ReferenceText_mc;
   var bNote;
   var iCurrentLine;
   var iLeftPageNumber;
   var iMaxPageHeight;
   var iNextPageBreak;
   var iPageSetIndex;
   var iPaginationIndex;
   static var BookMenuInstance;
   static var PAGE_BREAK_TAG = "[pagebreak]";
   static var NOTE_WIDTH = 400;
   static var NOTE_X_OFFSET = 20;
   static var NOTE_Y_OFFSET = 10;
   static var CACHED_PAGES = 4;
   static var EDIT_FIELD_HEIGHT = 20000;   // ~40 note pages before the edit field would scroll
   static var EDIT_MASK_OVERHANG = 12;
   static var EDIT_KEY_TURN_BLOCK_MS = 200;
   // Text sizes for books and notes: the page text field's own size (vanilla 22,
   // Convenient Reading 19), overridden by Convenient Reading.ini when it exists.
   static var FONT_SIZE_B = 18;
   static var FONT_SIZE_N = 18;

   // ---- Edit mode properties ----
   var bEditMode;
   var iEditPage;        // page the caret is on
   var aEditPageTops;    // y of each page's first line in EditField (same rule as CalculatePagination), then the text's bottom
   var aEditPageLines;   // index of each page's first line
   var EditClip;         // Visible MovieClip used for editing (duplicated from ReferenceText_mc)
   var EditField;        // The TextField inside EditClip: the whole text, tall enough never to scroll
   var EditMask;         // shows one page of EditField
   var iSuppressTurnUntil;   // getTimer() before which engine page turns are refused (key presses)
   var iEditShownFrom;   // books: offset of the engine's current spread in its 4 page slots (0 or 2)
   var iShowCalls;       // DIAGNOSTIC: ShowPageAtOffset calls in edit mode
   var iLastShowOffset;  // DIAGNOSTIC
   var sTurnLog;         // DIAGNOSTIC: engine page turns in edit mode (delta, and whether it turned)
   var bTextReceived;    // the game has sent this book's text (SetBookText): note/book is known
   var bEditPending;     // edit mode was requested before that: enter it when the text arrives

   function BookMenu()
   {
      super();
      BookMenu.BookMenuInstance = this;
      this.BookPages = new Array();
      this.PageInfoA = new Array();
      this.iLeftPageNumber = 0;
      this.iPageSetIndex = 0;
      this.bNote = false;
      this.bEditMode = false;
      this.iEditPage = 0;
      this.aEditPageTops = [2];
      this.ReferenceText_mc = this.ReferenceTextInstance;
      this.RefTextFieldTextFormat = this.ReferenceText_mc.PageTextField.getTextFormat();
      BookMenu.FONT_SIZE_B = BookMenu.FONT_SIZE_N = BookMenu.ValidSize(this.RefTextFieldTextFormat.size, 18);
      var _loc3_ = new LoadVars();
      _loc3_.load("../Convenient Reading.ini");
      // Without the ini this can still be called, with nothing usable: keep the defaults.
      _loc3_.onData = function(str)
      {
         BookMenu.FONT_SIZE_B = BookMenu.ValidSize(parseFloat(BookMenu.ParseConfig(str,"iBookFontSize")), BookMenu.FONT_SIZE_B);
         BookMenu.FONT_SIZE_N = BookMenu.ValidSize(parseFloat(BookMenu.ParseConfig(str,"iNoteFontSize")), BookMenu.FONT_SIZE_N);
      };
   }

   // A usable text size, or the fallback (NaN or 0 would make text invisible).
   static function ValidSize(size, fallback)
   {
      return size == undefined || isNaN(size) || size <= 0 ? fallback : size;
   }

   function onLoad()
   {
      this.ReferenceText_mc._visible = false;
      this.ReferenceTextField = this.ReferenceText_mc.PageTextField;
      this.ReferenceTextField.noTranslate = true;
      this.ReferenceTextField.setTextFormat(this.RefTextFieldTextFormat);
      this.iMaxPageHeight = this.ReferenceTextField._height;
      var fieldSize = this.ReferenceTextField.getTextFormat().size;
      BookMenu.FONT_SIZE_B = BookMenu.ValidSize(BookMenu.FONT_SIZE_B, fieldSize);
      BookMenu.FONT_SIZE_N = BookMenu.ValidSize(BookMenu.FONT_SIZE_N, fieldSize);

      // Original callbacks
      gfx.io.GameDelegate.addCallBack("SetBookText",this,"SetBookText");
      gfx.io.GameDelegate.addCallBack("TurnPage",this,"TurnPage");
      gfx.io.GameDelegate.addCallBack("PrepForClose",this,"PrepForClose");

      // New edit mode callbacks
      gfx.io.GameDelegate.addCallBack("SetEditMode",this,"SetEditMode");
      gfx.io.GameDelegate.addCallBack("GetLetterText",this,"onGetLetterText");
   }

   // ================================================================
   // EDIT MODE
   // ================================================================

   function SetEditMode(abEnabled)
   {
      if(abEnabled && !this.bTextReceived)
      {
         // The layout depends on note vs book, which SetBookText tells us.
         this.bEditPending = true;
         return;
      }
      this.bEditMode = abEnabled;
      if(abEnabled)
      {
         this.EnterEditMode();
      }
      else
      {
         this.ExitEditMode();
      }
   }

   function EnterEditMode()
   {
      // Hide ALL existing display pages
      var i = 0;
      while(i < this.BookPages.length)
      {
         this.BookPages[i]._visible = false;
         i++;
      }

      // Stop any ongoing pagination
      if(this.iPaginationIndex != -1)
      {
         clearInterval(this.iPaginationIndex);
         this.iPaginationIndex = -1;
      }

      // Duplicate the ReferenceText_mc to get a clip with the correct font embedding
      this.EditClip = this.ReferenceText_mc.duplicateMovieClip("EditClip", this.getNextHighestDepth());
      // Vanilla's reference clip has two frames and would keep looping in the copy.
      this.EditClip.gotoAndStop(1);
      this.EditField = this.EditClip.PageTextField;

      // Clear ALL text from the duplicated field (it copied the note's content)
      this.EditField.text = "";
      this.EditField.htmlText = "";
      this.EditField.replaceText(0, this.EditField.length, "");

      // Make it editable
      this.EditField.type = "input";
      this.EditField.selectable = true;
      this.EditField.noTranslate = true;
      this.EditField.multiline = true;
      this.EditField.wordWrap = true;
      this.EditField.autoSize = "none";
      this.EditField.verticalAutoSize = "none";

      // Apply handwriting font formatting
      var fmt = new TextFormat();
      fmt.font = "$HandwrittenFont";
      fmt.size = this.bNote ? BookMenu.FONT_SIZE_N : BookMenu.FONT_SIZE_B;
      fmt.color = 0x1B1410;
      fmt.letterSpacing = 0;
      fmt.kerning = true;
      this.EditField.setNewTextFormat(fmt);
      this.EditField.setTextFormat(fmt);

      // Position to match note layout (same as CreateDisplayPage does for notes)
      if(this.bNote)
      {
         this.EditField._width = BookMenu.NOTE_WIDTH;
         this.EditClip._x = Stage.visibleRect.x + BookMenu.NOTE_X_OFFSET;
         this.EditClip._y = Stage.visibleRect.y + BookMenu.NOTE_Y_OFFSET;
      }
      else
      {
         this.EditClip._x = this.ReferenceText_mc._x;
         this.EditClip._y = this.ReferenceText_mc._y;
      }
      // The whole text in one field that never scrolls (a scrolling input field
      // follows the caret a line at a time); the mask shows one page of it.
      this.EditField._height = BookMenu.EDIT_FIELD_HEIGHT;
      this.EditMask = this.createEmptyMovieClip("EditMask", this.getNextHighestDepth());
      this.EditClip.setMask(this.EditMask);
      this.EditClip._visible = true;

      this.iEditPage = 0;
      this.iEditShownFrom = 0;
      this.iShowCalls = 0;
      this.sTurnLog = "";
      Selection.setFocus(this.EditField);
      Selection.setSelection(0, 0);
      this.EditLayout();
   }

   // ---- Pages ----

   // Recompute the page tops and show the caret's page. Call after every edit or caret move.
   function EditLayout()
   {
      if(this.EditField == undefined)
      {
         return undefined;
      }
      this.EditField.scroll = 1;
      var tf = this.EditField;
      var tops = [2];
      var firstLines = [0];
      var y = 2;   // Flash's text gutter: line 0 starts 2px down
      var caretLine = this.EditCaretLine();
      var caretPage = 0;
      var i = 0;
      while(i < tf.numLines)
      {
         var m = tf.getLineMetrics(i);
         // Same rule as CalculatePagination: a line whose bottom passes the page starts a new page.
         if(i > 0 && y + m.ascent + m.descent > tops[tops.length - 1] + this.iMaxPageHeight)
         {
            tops.push(y);
            firstLines.push(i);
         }
         if(i == caretLine)
         {
            caretPage = tops.length - 1;
         }
         y += m.height;
         i++;
      }
      tops.push(y);   // end of the text: bottom of the last page
      this.aEditPageTops = tops;
      this.aEditPageLines = firstLines;
      this.iEditPage = caretPage;
      this.ShowEditPage(caretPage);
   }

   function EditPageCount()
   {
      return this.aEditPageTops.length - 1;
   }

   function EditCaretLine()
   {
      var pos = Selection.getBeginIndex();
      var tf = this.EditField;
      if(pos < 0 || pos >= tf.length)
      {
         return tf.numLines - 1;
      }
      return tf.getLineIndexOfChar(pos);
   }

   // Move the field so page p sits where page 0 would, and mask everything else.
   function ShowEditPage(p)
   {
      var last = this.EditPageCount() - 1;
      if(p > last)
      {
         p = last;
      }
      if(p < 0)
      {
         p = 0;
      }
      var top = this.aEditPageTops[p];
      var bottom = p < last ? this.aEditPageTops[p + 1] : top + this.iMaxPageHeight;
      this.EditField._y = 2 - top;
      this.EditClip._visible = true;
      var m = this.EditMask;
      m.clear();
      m._x = this.EditClip._x;
      m._y = this.EditClip._y;
      m.beginFill(0xFF0000, 100);
      // Wider than the field: handwritten glyphs overhang its edges.
      var left = - BookMenu.EDIT_MASK_OVERHANG;
      var right = this.EditField._width + BookMenu.EDIT_MASK_OVERHANG;
      var h = bottom - top + 2;
      m.moveTo(left, 0);
      m.lineTo(right, 0);
      m.lineTo(right, h);
      m.lineTo(left, h);
      m.lineTo(left, 0);
      m.endFill();
   }

   // Put the caret on page p (its first character) and show it.
   function EditGoToPage(p)
   {
      if(p < 0 || p >= this.EditPageCount())
      {
         return false;
      }
      var pos = this.EditField.getLineOffset(this.aEditPageLines[p]);
      if(pos < 0)
      {
         pos = this.EditField.length;
      }
      Selection.setFocus(this.EditField);
      Selection.setSelection(pos, pos);
      this.EditLayout();
      return true;
   }

   // After the plugin forwards a click: the caret may have moved.
   function EditRefresh()
   {
      this.EditLayout();
   }

   function AppendEditChar(ch)
   {
      if(this.EditField != undefined)
      {
         // Insert at cursor position
         var pos = Selection.getBeginIndex();
         if(pos < 0) pos = this.EditField.text.length;
         this.EditField.replaceText(pos, pos, ch);
         // Move cursor after inserted character
         Selection.setFocus(this.EditField);
         Selection.setSelection(pos + 1, pos + 1);
      }
      this.EditLayout();
   }

   function EditBackspace()
   {
      if(this.EditField != undefined)
      {
         // Delete character before cursor position
         var pos = Selection.getBeginIndex();
         if(pos > 0)
         {
            this.EditField.replaceText(pos - 1, pos, "");
            Selection.setFocus(this.EditField);
            Selection.setSelection(pos - 1, pos - 1);
         }
      }
      this.EditLayout();
   }

   function EditMoveCursor(direction)
   {
      if(this.EditField != undefined)
      {
         var pos = Selection.getBeginIndex();
         var len = this.EditField.text.length;
         if(direction == "left" && pos > 0)
         {
            Selection.setFocus(this.EditField);
            Selection.setSelection(pos - 1, pos - 1);
         }
         else if(direction == "right" && pos < len)
         {
            Selection.setFocus(this.EditField);
            Selection.setSelection(pos + 1, pos + 1);
         }
         else if(direction == "home")
         {
            Selection.setFocus(this.EditField);
            Selection.setSelection(0, 0);
         }
         else if(direction == "end")
         {
            Selection.setFocus(this.EditField);
            Selection.setSelection(len, len);
         }
         else if(direction == "up")
         {
            // Move cursor up one line
            var curLine = this.EditField.getLineIndexOfChar(pos);
            if(curLine > 0)
            {
               var lineStart = this.EditField.getLineOffset(curLine);
               var colOffset = pos - lineStart;
               var prevLineStart = this.EditField.getLineOffset(curLine - 1);
               var prevLineEnd = lineStart - 1;
               var prevLineLen = prevLineEnd - prevLineStart;
               var newPos = prevLineStart + Math.min(colOffset, prevLineLen);
               Selection.setFocus(this.EditField);
               Selection.setSelection(newPos, newPos);
            }
         }
         else if(direction == "down")
         {
            // Move cursor down one line
            var curLine2 = this.EditField.getLineIndexOfChar(pos);
            if(curLine2 < this.EditField.numLines - 1)
            {
               var lineStart2 = this.EditField.getLineOffset(curLine2);
               var colOffset2 = pos - lineStart2;
               var nextLineStart = this.EditField.getLineOffset(curLine2 + 1);
               var nextNextLineStart = this.EditField.getLineOffset(curLine2 + 2);
               var nextLineLen = (nextNextLineStart >= 0 ? nextNextLineStart : len) - nextLineStart;
               var newPos2 = nextLineStart + Math.min(colOffset2, nextLineLen);
               Selection.setFocus(this.EditField);
               Selection.setSelection(newPos2, newPos2);
            }
         }
      }
      this.EditLayout();
   }

   function EditDelete()
   {
      if(this.EditField != undefined)
      {
         var pos = Selection.getBeginIndex();
         if(pos >= 0 && pos < this.EditField.text.length)
         {
            this.EditField.replaceText(pos, pos + 1, "");
            Selection.setFocus(this.EditField);
            Selection.setSelection(pos, pos);
         }
      }
      this.EditLayout();
   }

   function ExitEditMode()
   {
      if(this.EditClip != undefined)
      {
         this.EditClip.setMask(null);
         this.EditClip.removeMovieClip();
         this.EditClip = undefined;
         this.EditField = undefined;
      }
      if(this.EditMask != undefined)
      {
         this.EditMask.removeMovieClip();
         this.EditMask = undefined;
      }
      this.bEditMode = false;
      this.iEditPage = 0;
   }

   function onGetLetterText()
   {
      skse.SendModEvent("SNPD_LetterText", this.EditField.text);
   }

   // Override TurnPage to handle edit mode page flipping
   function TurnPage(aiDelta)
   {
      if(this.bEditMode)
      {
         return this.EditTurnPage(aiDelta);
      }
      // ---- Original TurnPage logic ----
      var _loc2_ = this.iLeftPageNumber + aiDelta;
      var _loc3_ = _loc2_ >= 0 && _loc2_ < this.PageInfoA.length;
      if(this.bNote)
      {
         _loc3_ = _loc2_ >= 0 && _loc2_ < this.PageInfoA.length - 1;
      }
      var _loc4_ = Math.abs(aiDelta);
      var _loc5_;
      if(_loc3_)
      {
         _loc5_ = _loc4_ != 1 ? 4 : 1;
         this.SetLeftPageNumber(_loc2_);
         if(this.iLeftPageNumber < this.iPageSetIndex)
         {
            this.iPageSetIndex -= _loc4_;
         }
         else if(this.iLeftPageNumber >= this.iPageSetIndex + _loc5_)
         {
            this.iPageSetIndex += _loc4_;
         }
         this.UpdatePages();
      }
      return _loc3_;
   }

   // The engine's page turn (a click; the plugin keeps the arrow keys from it). Books turn a
   // spread, notes a page. Only to a page that exists: false skips the turn animation.
   function EditTurnPage(aiDelta)
   {
      if(getTimer() < this.iSuppressTurnUntil)
      {
         this.sTurnLog = (this.sTurnLog == undefined ? "" : this.sTurnLog + ",") + aiDelta + "blocked";
         return false;
      }
      var from = this.bNote ? this.iEditPage : this.EditSpreadLeft();
      var ok = this.EditGoToPage(from + aiDelta);
      if(ok && !this.bNote)
      {
         // Vanilla TurnPage's page window: after a forward turn the engine shows the spread in
         // slots 2-3 (the turned leaf's far side), after a backward turn in slots 0-1.
         this.iEditShownFrom = aiDelta > 0 ? Math.abs(aiDelta) : 0;
      }
      this.sTurnLog = (this.sTurnLog == undefined ? "" : this.sTurnLog + ",") + aiDelta + (ok ? "ok" : "no");
      return ok;
   }

   // The plugin calls this on every key event while editing (press, held repeat, release): the
   // page keys (arrows, A, D) type or move the caret, so a turn while keys are in use is theirs,
   // not a click's.
   function EditSuppressTurn()
   {
      this.iSuppressTurnUntil = getTimer() + BookMenu.EDIT_KEY_TURN_BLOCK_MS;
   }

   // Left page of the spread the caret is on (books show pages in pairs).
   function EditSpreadLeft()
   {
      return this.iEditPage - this.iEditPage % 2;
   }

   // ---- DIAGNOSTIC: the edit mode's display state, for the plugin log ----
   static function DescribeClip(label, mc)
   {
      if(mc == undefined)
      {
         return label + "=undefined";
      }
      return label + "{" + mc._target + " depth=" + mc.getDepth() + " x=" + mc._x + " y=" + mc._y + " w=" + mc._width + " h=" + mc._height + " xs=" + mc._xscale + " ys=" + mc._yscale + " vis=" + mc._visible + " a=" + mc._alpha + " frame=" + mc._currentframe + "/" + mc._totalframes + " blend=" + mc.blendMode + " filters=" + (mc.filters == undefined ? "none" : mc.filters.length) + " bmp=" + mc.cacheAsBitmap + "}";
   }

   static function DescribeField(label, tf)
   {
      if(tf == undefined)
      {
         return label + "=undefined";
      }
      var fmt = tf.getTextFormat();
      var nfmt = tf.getNewTextFormat();
      return label + "{x=" + tf._x + " y=" + tf._y + " w=" + tf._width + " h=" + tf._height + " vis=" + tf._visible + " a=" + tf._alpha + " len=" + tf.text.length + " html=" + tf.htmlText.length + " tw=" + tf.textWidth + " th=" + tf.textHeight + " scroll=" + tf.scroll + "/" + tf.maxscroll + " bottom=" + tf.bottomScroll + " lines=" + tf.numLines + " type=" + tf.type + " embed=" + tf.embedFonts + " color=" + tf.textColor.toString(16) + " wrap=" + tf.wordWrap + " multi=" + tf.multiline + " auto=" + tf.autoSize + " fmt=" + fmt.font + "/" + fmt.size + "/" + (fmt.color == undefined ? "?" : fmt.color.toString(16)) + " newfmt=" + nfmt.font + "/" + nfmt.size + " text='" + tf.text.substring(0, 40) + "'}";
   }

   static function DescribeRuns(tf)
   {
      if(tf == undefined || tf.length == 0)
      {
         return "";
      }
      var out = "";
      var key = "";
      var i = 0;
      while(i < tf.length)
      {
         var f = tf.getTextFormat(i);
         var k = f.font + "/" + f.size + "/lead" + f.leading;
         if(k != key)
         {
            out += (out.length ? " " : "") + i + ":" + k;
            key = k;
         }
         i++;
      }
      return out;
   }

   function DebugState()
   {
      var s = "note=" + this.bNote + " edit=" + this.bEditMode + " pending=" + this.bEditPending + " textIn=" + this.bTextReceived + " sizes=" + BookMenu.FONT_SIZE_B + "/" + BookMenu.FONT_SIZE_N + " editPage=" + this.iEditPage + " maxPageH=" + this.iMaxPageHeight + " left=" + this.iLeftPageNumber + " set=" + this.iPageSetIndex + " pageInfo=" + this.PageInfoA.length + " pagination=" + this.iPaginationIndex;
      s += " | editPages=" + this.EditPageCount() + " tops=" + this.aEditPageTops.join(",") + " firstLines=" + this.aEditPageLines.join(",") + " shows=" + this.iShowCalls + " lastShow=" + this.iLastShowOffset + " shownFrom=" + this.iEditShownFrom + " editPage=" + this.iEditPage + " turns=" + this.sTurnLog + (this.EditField == undefined ? "" : " fieldY=" + this.EditField._y + " maskY=" + this.EditMask._y + " maskH=" + this.EditMask._height + " clipVis=" + this.EditClip._visible);
      s += " | stage{w=" + Stage.width + " h=" + Stage.height + " mode=" + Stage.scaleMode + " rect=" + Stage.visibleRect.x + "," + Stage.visibleRect.y + "," + Stage.visibleRect.width + "," + Stage.visibleRect.height + "}";
      s += " | focus=" + Selection.getFocus() + " caret=" + Selection.getBeginIndex();
      s += " | " + BookMenu.DescribeClip("menu", this);
      s += " | " + BookMenu.DescribeClip("ref", this.ReferenceText_mc) + " " + BookMenu.DescribeField("refField", this.ReferenceTextField);
      s += " | refRuns{" + BookMenu.DescribeRuns(this.ReferenceTextField) + "} refHtml{" + String(this.ReferenceTextField.htmlText).substr(0,2000) + "}";
      s += " | " + BookMenu.DescribeClip("edit", this.EditClip) + " " + BookMenu.DescribeField("editField", this.EditField);
      var i = 0;
      while(i < this.BookPages.length)
      {
         s += " | page" + this.BookPages[i].pageNum + ":" + BookMenu.DescribeClip("", this.BookPages[i]);
         i++;
      }
      for(var name in this)
      {
         if(typeof this[name] == "movieclip")
         {
            s += " | child " + name + " depth=" + this[name].getDepth() + " vis=" + this[name]._visible;
         }
      }
      return s;
   }

   // Override PrepForClose to handle edit mode cleanup
   function PrepForClose()
   {
      if(this.bEditMode)
      {
         this.onGetLetterText();
         this.ExitEditMode();
         return undefined;
      }
      this.iPageSetIndex = this.iLeftPageNumber;
   }

   // ================================================================
   // ORIGINAL METHODS (unchanged)
   // ================================================================

   static function trim(str)
   {
      var _loc2_ = 0;
      var _loc1_ = str.length - 1;
      while(str.charCodeAt(_loc2_) < 33)
      {
         _loc2_ = _loc2_ + 1;
      }
      while(str.charCodeAt(_loc1_) < 33)
      {
         _loc1_ = _loc1_ - 1;
      }
      return str.substring(_loc2_,_loc1_ + 1);
   }

   static function ParseConfig(str, par)
   {
      var _loc3_ = str.split("\n");
      var _loc4_ = 0;
      var _loc5_ = 0;
      var _loc6_;
      var _loc7_;
      var _loc8_;
      var _loc9_;
      while(_loc4_ < _loc3_.length)
      {
         if(_loc3_[_loc4_].charAt(0) != "#" && _loc3_[_loc4_].charAt(0) != "[")
         {
            _loc6_ = BookMenu.trim(_loc3_[_loc4_]);
            _loc7_ = _loc6_.indexOf("=");
            _loc8_ = _loc6_.substring(0,_loc7_);
            _loc9_ = BookMenu.trim(_loc8_);
            if(_loc9_ == par)
            {
               _loc5_ = _loc4_;
               break;
            }
         }
         _loc4_ += 1;
      }
      var _loc10_ = BookMenu.trim(_loc3_[_loc5_]);
      var _loc11_ = _loc10_.indexOf("=");
      var _loc12_ = _loc10_.substring(_loc11_ + 1,_loc10_.length);
      return BookMenu.trim(_loc12_);
   }

   function SetBookText(astrText, abNote)
   {
      this.bNote = abNote;
      this.bTextReceived = true;
      if(this.bEditPending)
      {
         this.bEditPending = false;
         this.bEditMode = true;
         this.EnterEditMode();
         return;
      }
      // Don't overwrite text while in edit mode
      if(this.bEditMode)
      {
         return;
      }
      this.ReferenceTextField.verticalAutoSize = "top";
      if(abNote)
      {
         this.ReferenceTextField.SetText("<font size=\'" + BookMenu.FONT_SIZE_N + "\'>" + astrText + "</font>",true);
         this.ReferenceTextField._width = BookMenu.NOTE_WIDTH;
      }
      else
      {
         this.ReferenceTextField.SetText("<font size=\'" + BookMenu.FONT_SIZE_B + "\'>" + astrText + "</font>",true);
      }
      this.PageInfoA.push({pageTop:0,pageHeight:this.iMaxPageHeight});
      this.iCurrentLine = 0;
      this.iPaginationIndex = setInterval(this,"CalculatePagination",30);
      this.iNextPageBreak = this.iMaxPageHeight;
      this.SetLeftPageNumber(0);
   }

   function CreateDisplayPage(PageTop, PageBottom, aPageNum)
   {
      var _loc2_ = this.ReferenceText_mc.duplicateMovieClip("Page",this.getNextHighestDepth());
      var _loc3_ = _loc2_.PageTextField;
      _loc3_.noTranslate = true;
      _loc3_.SetText(this.ReferenceTextField.htmlText,true);
      var _loc4_ = this.ReferenceTextField.getLineOffset(this.ReferenceTextField.getLineIndexAtPoint(0,PageTop));
      var _loc5_ = this.ReferenceTextField.getLineOffset(this.ReferenceTextField.getLineIndexAtPoint(0,PageBottom));
      _loc3_.replaceText(0,_loc4_,"");
      _loc3_.replaceText(_loc5_ - _loc4_,this.ReferenceTextField.length,"");
      _loc3_.autoSize = "left";
      if(this.bNote)
      {
         _loc3_._width = BookMenu.NOTE_WIDTH;
         _loc2_._x = Stage.visibleRect.x + BookMenu.NOTE_X_OFFSET;
         _loc2_._y = Stage.visibleRect.y + BookMenu.NOTE_Y_OFFSET;
      }
      else
      {
         _loc2_._x = this.ReferenceText_mc._x;
         _loc2_._y = this.ReferenceText_mc._y;
      }
      _loc2_._visible = false;
      _loc2_.pageNum = aPageNum;
      this.BookPages.push(_loc2_);
   }

   function CalculatePagination()
   {
      var _loc9_ = false;
      var _loc5_;
      var _loc6_;
      var _loc4_;
      var _loc3_;
      var _loc2_;
      while(this.iCurrentLine <= this.ReferenceTextField.numLines)
      {
         _loc5_ = this.ReferenceTextField.getLineOffset(this.iCurrentLine);
         _loc6_ = this.ReferenceTextField.getLineOffset(this.iCurrentLine + 1);
         _loc4_ = this.ReferenceTextField.getCharBoundaries(_loc5_);
         _loc3_ = _loc6_ != -1 ? this.ReferenceTextField.text.substring(_loc5_,_loc6_) : this.ReferenceTextField.text.substring(_loc5_);
         _loc3_ = Shared.GlobalFunc.StringTrim(_loc3_);
         if(_loc4_.bottom > this.iNextPageBreak || _loc3_ == BookMenu.PAGE_BREAK_TAG || this.iCurrentLine >= this.ReferenceTextField.numLines)
         {
            _loc2_ = {pageTop:0,pageHeight:this.iMaxPageHeight};
            if(_loc3_ == BookMenu.PAGE_BREAK_TAG)
            {
               _loc2_.pageTop = _loc4_.bottom + this.ReferenceTextField.getLineMetrics(this.iCurrentLine).leading;
               this.PageInfoA[this.PageInfoA.length - 1].pageHeight = _loc4_.top - this.PageInfoA[this.PageInfoA.length - 1].pageTop;
            }
            else
            {
               _loc2_.pageTop = _loc4_.top;
               this.PageInfoA[this.PageInfoA.length - 1].pageHeight = _loc2_.pageTop - this.PageInfoA[this.PageInfoA.length - 1].pageTop;
            }
            this.iNextPageBreak = _loc2_.pageTop + this.iMaxPageHeight;
            if(_loc2_.pageTop != undefined || this.bNote)
            {
               this.PageInfoA.push(_loc2_);
            }
            _loc9_ = true;
         }
         this.iCurrentLine++;
      }
      if(this.iCurrentLine >= this.ReferenceTextField.numLines)
      {
         clearInterval(this.iPaginationIndex);
         this.iPaginationIndex = -1;
      }
      this.UpdatePages();
   }

   function SetLeftPageNumber(aiPageNum)
   {
      if(aiPageNum < this.PageInfoA.length)
      {
         this.iLeftPageNumber = aiPageNum;
      }
   }

   function ShowPageAtOffset(aiPageOffset)
   {
      if(this.bEditMode && this.EditField != undefined)
      {
         // The engine draws each side of the open book by calling this with 0, then 1.
         this.iShowCalls++;
         this.iLastShowOffset = aiPageOffset;
         // Books: the engine's slots 0-3 are a window of pages; the current spread sits at
         // slots iEditShownFrom and +1, the other two are the far side of a turning leaf.
         var p = this.bNote ? this.iEditPage : this.EditSpreadLeft() - this.iEditShownFrom + aiPageOffset;
         this.EditClip._visible = p >= 0 && p < this.EditPageCount();
         if(this.EditClip._visible)
         {
            this.ShowEditPage(p);
         }
         return undefined;
      }
      var _loc2_ = 0;
      while(_loc2_ < this.BookPages.length)
      {
         if(this.BookPages[_loc2_].pageNum == this.iPageSetIndex + aiPageOffset)
         {
            this.BookPages[_loc2_]._visible = true;
         }
         else
         {
            this.BookPages[_loc2_]._visible = false;
         }
         _loc2_ = _loc2_ + 1;
      }
   }

   function UpdatePages()
   {
      var _loc4_ = 0;
      var _loc3_;
      var _loc2_;
      while(_loc4_ < BookMenu.CACHED_PAGES)
      {
         _loc3_ = false;
         _loc2_ = 0;
         while(_loc2_ < this.BookPages.length && !_loc3_)
         {
            if(this.BookPages[_loc2_].pageNum == this.iPageSetIndex + _loc4_)
            {
               _loc3_ = true;
            }
            _loc2_ = _loc2_ + 1;
         }
         if(!_loc3_ && (this.PageInfoA.length > this.iPageSetIndex + _loc4_ + 1 || this.iPaginationIndex == -1 && this.PageInfoA.length > this.iPageSetIndex + _loc4_))
         {
            this.CreateDisplayPage(this.PageInfoA[this.iPageSetIndex + _loc4_].pageTop,this.PageInfoA[this.iPageSetIndex + _loc4_].pageTop + this.PageInfoA[this.iPageSetIndex + _loc4_].pageHeight,this.iPageSetIndex + _loc4_);
         }
         _loc4_ = _loc4_ + 1;
      }
      _loc4_ = 0;
      while(_loc4_ < this.BookPages.length)
      {
         if(this.BookPages[_loc4_].pageNum < this.iPageSetIndex || this.BookPages[_loc4_].pageNum >= this.iPageSetIndex + BookMenu.CACHED_PAGES)
         {
            this.BookPages.splice(_loc4_,1)[0].removeMovieClip();
         }
         _loc4_ = _loc4_ + 1;
      }
   }
}
