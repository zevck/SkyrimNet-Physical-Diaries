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
   var aSegs;            // the text as segments: {locked, body, editable} lengths, in order (see EditBuildContent)
   var oEditContent;     // what the plugin sent (SetEditContent) for the next edit mode, or undefined: blank page
   var oContentFmt;      // entry text's format (config font, content size)
   var oBreakFmt;        // blank lines' format, as reading has them (see EditBuildContent)
   var bTextReceived;    // SetBookText has run (EditReady)
   var iDatesStart;      // the title page's date range in EditField (see EditSetDates)
   var iDatesLength;

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
   }

   // ================================================================
   // EDIT MODE (docs/EDITING.md). The plugin calls these through the movie; the book is open,
   // so SetBookText has run and bNote is known.
   // ================================================================

   function EnterEditMode()
   {
      this.bEditMode = true;
      // The page being read (at the book's opening: page 0). A book's spread is in engine
      // slots 0-1 or 2-3 (see iEditShownFrom); editing starts on the same page and slots.
      var readPage = this.iLeftPageNumber;
      var readShownFrom = this.bNote ? 0 : this.iLeftPageNumber - this.iPageSetIndex;
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
      // The page's own ink, as when reading (vanilla black; the Convenient Reading variant's own).
      fmt.color = this.RefTextFieldTextFormat.color == undefined ? 0 : this.RefTextFieldTextFormat.color;
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
      this.EditBuildContent(fmt);
      var start = this.EditSnap(0, 1);
      this.EditSetCaretOrNone(start);
      this.EditLayout();
      // Start on the page being read. If it has nowhere to type (the title spread), turn to
      // the first entry's page, where the caret is; with no entry at all, stay.
      this.iEditShownFrom = readShownFrom;
      this.EditGoToPage(readPage);
      if(start >= 0)
      {
         var caretPage = this.PageOfPos(this.EditCaret());
         if(caretPage != this.iEditPage && (this.bNote || caretPage != this.iEditPage + 1))
         {
            this.EditLayout();
         }
      }
   }

   // ---- Content: locked text and editable bodies ----

   // From the plugin, before EnterEditMode: a diary volume laid out as the book shows it. Title
   // and dates go on the title page; entries is "heading\x1Fbody" per entry, joined by \x1E.
   // Headings are locked; each body is editable (entry i of EditGetBodies).
   function SetEditContent(font, titleSize, smallSize, dateSize, contentSize, title, dates, entries)
   {
      this.oEditContent = {font:font, titleSize:titleSize, smallSize:smallSize, dateSize:dateSize, contentSize:contentSize, title:title, dates:dates, entries:entries.length ? entries.split(String.fromCharCode(30)) : []};
   }

   static function FieldText(str)
   {
      return str.split("\r\n").join("\r").split("\n").join("\r");
   }

   // Fill EditField and aSegs with FormatDiaryEntries' layout as the book menu lays it out, so
   // each page matches the reading view line for line. Every segment after the first starts
   // with a locked "\r" and a new page, the "[pagebreak]" line's end. Line breaks are where
   // reading differs from plain text: see FormatBreaks. Without content: one editable
   // segment, the blank page.
   function EditBuildContent(baseFmt)
   {
      var c = this.oEditContent;
      if(c == undefined)
      {
         this.aSegs = [{locked:0, body:0, editable:true}];
         this.EditField.text = "";
         return undefined;
      }
      var text = "";
      var styles = [];   // {start, end, size, align}
      var segs = [];
      // Blank first page, then the title page: seven blank lines, the title, two blank
      // lines, the date range.
      segs.push({locked:0, body:0, editable:false});
      var title = "\r\r\r\r\r\r\r\r";
      var tStart = title.length;
      title += BookMenu.FieldText(c.title);
      styles.push({start:tStart, end:title.length, size:c.titleSize, align:"center"});
      title += "\r\r\r";
      var dStart = title.length;
      title += BookMenu.FieldText(c.dates);
      this.iDatesStart = dStart;   // the title page comes first, so this is also the field offset
      this.iDatesLength = title.length - dStart;
      styles.push({start:dStart, end:title.length, size:c.smallSize, align:"center"});
      segs.push({locked:title.length, body:0, editable:false});
      text += title;
      var i = 0;
      while(i < c.entries.length)
      {
         var parts = c.entries[i].split(String.fromCharCode(31));
         var heading = BookMenu.FieldText(parts[0]);
         var body = BookMenu.FieldText(parts[1] == undefined ? "" : parts[1]);
         // An entry's page: a blank line, then the heading and a blank line (if headings are on).
         var locked = "\r\r";
         if(heading.length)
         {
            styles.push({start:text.length + 2, end:text.length + 2 + heading.length, size:c.dateSize, align:"left"});
            locked += heading + "\r\r";
         }
         segs.push({locked:locked.length, body:body.length, editable:true});
         text += locked + body;
         i++;
      }
      this.EditField.text = text;
      baseFmt.font = c.font;
      baseFmt.size = c.contentSize;
      baseFmt.align = "left";
      this.EditField.setTextFormat(baseFmt);
      this.EditField.setNewTextFormat(baseFmt);
      i = 0;
      while(i < styles.length)
      {
         var f = new TextFormat();
         f.size = styles[i].size;
         f.align = styles[i].align;
         if(styles[i].end > styles[i].start)
         {
            this.EditField.setTextFormat(styles[i].start, styles[i].end, f);
         }
         i++;
      }
      this.aSegs = segs;
      this.oContentFmt = baseFmt;
      // The page field's own font and the book text's outer size: what reading gives the line
      // breaks outside SNPD's font tags (the page's default font, in BookMenu's size wrapper).
      this.oBreakFmt = new TextFormat();
      this.oBreakFmt.font = this.RefTextFieldTextFormat.font;
      this.oBreakFmt.size = this.bNote ? BookMenu.FONT_SIZE_N : BookMenu.FONT_SIZE_B;
      var k = 0;
      while(k < segs.length)
      {
         var start = this.SegStart(k);
         var j = start;
         while(j < start + segs[k].locked)
         {
            if(text.charAt(j) == "\r")
            {
               this.EditField.setTextFormat(j, j + 1, this.oBreakFmt);
            }
            j++;
         }
         if(segs[k].editable)
         {
            this.FormatBreaks(k);
         }
         k++;
      }
   }

   // Body k's formats as FormatDiaryEntries gives them: every paragraph (split on "\r\r") at the
   // content size, and each "\r\r" between paragraphs outside the font tags, in the page's font
   // and outer size, which sets a blank line's height. A single line break stays in its
   // paragraph. Re-run after every edit to the body.
   function FormatBreaks(k)
   {
      if(this.oBreakFmt == undefined)
      {
         return undefined;
      }
      var start = this.BodyStart(k);
      var end = this.BodyEnd(k);
      if(end <= start)
      {
         return undefined;
      }
      this.EditField.setTextFormat(start, end, this.oContentFmt);
      var text = this.EditField.text;
      var pos = start;
      while(true)
      {
         var found = text.indexOf("\r\r", pos);
         if(found < 0 || found + 2 > end)
         {
            break;
         }
         this.EditField.setTextFormat(found, found + 2, this.oBreakFmt);
         pos = found + 2;
      }
   }

   function SegStart(k)
   {
      var pos = 0;
      var j = 0;
      while(j < k)
      {
         pos += this.aSegs[j].locked + this.aSegs[j].body;
         j++;
      }
      return pos;
   }

   function BodyStart(k)
   {
      return this.SegStart(k) + this.aSegs[k].locked;
   }

   function BodyEnd(k)
   {
      return this.BodyStart(k) + this.aSegs[k].body;
   }

   // The editable segment whose body contains pos (either end included), or -1.
   function EditableSegAt(pos)
   {
      var k = 0;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable && pos >= this.BodyStart(k) && pos <= this.BodyEnd(k))
         {
            return k;
         }
         k++;
      }
      return -1;
   }

   // The nearest caret position in a body: pos itself if it is in one, else the nearest body
   // end before it (dir < 0) or body start after it (dir > 0), else the other way, else -1.
   function EditSnap(pos, dir)
   {
      if(this.EditableSegAt(pos) >= 0)
      {
         return pos;
      }
      var before = -1;
      var after = -1;
      var k = 0;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable)
         {
            if(this.BodyEnd(k) <= pos)
            {
               before = this.BodyEnd(k);
            }
            else if(after < 0 && this.BodyStart(k) >= pos)
            {
               after = this.BodyStart(k);
            }
         }
         k++;
      }
      if(dir < 0)
      {
         return before >= 0 ? before : after;
      }
      return after >= 0 ? after : before;
   }

   // The book's text has arrived (SetBookText, which comes after the menu opens): edit mode
   // can start.
   function EditReady()
   {
      return this.bTextReceived == true;
   }

   // A new, empty entry at the end: a new page with its heading (locked) and an empty body,
   // formatted as EditBuildContent does. The caret goes into it. Returns its index among the
   // entries, or -1.
   function EditAppendEntry(heading)
   {
      if(this.aSegs == undefined || this.EditField == undefined || this.oBreakFmt == undefined)
      {
         return -1;
      }
      var start = this.EditField.length;
      var h = BookMenu.FieldText(heading);
      var locked = "\r\r";
      if(h.length)
      {
         locked += h + "\r\r";
      }
      this.EditField.replaceText(start, start, locked);
      this.EditField.setTextFormat(start, start + locked.length, this.oContentFmt);
      var j = start;
      while(j < start + locked.length)
      {
         if(this.EditField.text.charAt(j) == "\r")
         {
            this.EditField.setTextFormat(j, j + 1, this.oBreakFmt);
         }
         j++;
      }
      if(h.length)
      {
         var f = new TextFormat();
         f.size = this.oEditContent.dateSize;
         f.align = "left";
         this.EditField.setTextFormat(start + 2, start + 2 + h.length, f);
      }
      this.aSegs.push({locked:locked.length, body:0, editable:true});
      this.EditSetCaret(this.EditField.length);
      this.EditLayout();
      var entries = 0;
      var k = 0;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable)
         {
            entries++;
         }
         k++;
      }
      return entries - 1;
   }

   // Put the caret at the end of entry i's text and show it.
   function EditFocusEntry(i)
   {
      var entry = -1;
      var k = 0;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable && ++entry == i)
         {
            this.EditSetCaret(this.BodyEnd(k));
            this.EditLayout();
            return true;
         }
         k++;
      }
      return false;
   }

   // The entry the caret is in (its index among the entries, as EditGetBodies orders them),
   // or -1.
   function EditCurrentEntry()
   {
      if(this.aSegs == undefined || this.EditField == undefined)
      {
         return -1;
      }
      var k = this.EditableSegAt(this.EditCaret());
      if(k < 0)
      {
         return -1;
      }
      var entry = 0;
      var j = 0;
      while(j < k)
      {
         if(this.aSegs[j].editable)
         {
            entry++;
         }
         j++;
      }
      return entry;
   }

   // Take entry i's pages out of the editor (heading and text): it was torn out. The caret
   // goes to the end of the entry before it, or the start of the one after.
   function EditRemoveEntry(i)
   {
      var k = 0;
      var entry = -1;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable && ++entry == i)
         {
            break;
         }
         k++;
      }
      if(k >= this.aSegs.length)
      {
         return false;
      }
      var start = this.SegStart(k);
      this.EditField.replaceText(start, start + this.aSegs[k].locked + this.aSegs[k].body, "");
      this.aSegs.splice(k, 1);
      var pos = this.EditSnap(start, -1);
      this.EditSetCaretOrNone(pos);
      this.EditLayout();
      return true;
   }

   // The caret at pos, or none at all when there is no entry to type in (pos -1): a caret on
   // the blank or title page would look editable.
   function EditSetCaretOrNone(pos)
   {
      if(pos < 0)
      {
         // Unfocusing alone still draws the caret: a field that can't be typed in draws none.
         Selection.setFocus(null);
         this.EditField.type = "dynamic";
         this.EditField.selectable = false;
      }
      else
      {
         this.EditSetCaret(pos);
      }
   }

   // The title page's date range, after entries were added or torn out (FormatDiaryEntries'
   // TitlePageDates, from the plugin).
   function EditSetDates(dates)
   {
      if(this.aSegs == undefined || this.aSegs.length < 2 || this.iDatesStart == undefined)
      {
         return false;
      }
      var text = BookMenu.FieldText(dates);
      var delta = text.length - this.iDatesLength;
      var hadFocus = Selection.getFocus() != null;
      var caret = this.EditCaret();
      this.EditField.replaceText(this.iDatesStart, this.iDatesStart + this.iDatesLength, text);
      if(text.length)
      {
         this.EditField.setTextFormat(this.iDatesStart, this.iDatesStart + text.length, this.oContentFmt);
         var f = new TextFormat();
         f.size = this.oEditContent.smallSize;
         f.align = "center";
         this.EditField.setTextFormat(this.iDatesStart, this.iDatesStart + text.length, f);
      }
      this.iDatesLength = text.length;
      this.aSegs[1].locked += delta;
      if(hadFocus && caret > this.iDatesStart)
      {
         this.EditSetCaret(caret + delta);
      }
      this.EditLayout();
      return true;
   }

   // Leave edit mode and read again, on the spread being edited, with the book's text as
   // it is now (the plugin renders it from the saved entries). Lays the text out as the
   // engine's SetBookText does, keeping the engine's page slots where they are.
   function ReturnToReading(text)
   {
      var page = this.iEditPage;
      var left = this.bNote ? page : page - page % 2;
      var setIndex = this.bNote ? page : left - this.iEditShownFrom;
      this.ExitEditMode();
      if(this.iPaginationIndex != -1)
      {
         clearInterval(this.iPaginationIndex);
         this.iPaginationIndex = -1;
      }
      while(this.BookPages.length)
      {
         this.BookPages.pop().removeMovieClip();
      }
      this.PageInfoA = new Array();
      this.SetBookText(text, this.bNote);
      this.iLeftPageNumber = left;
      this.iPageSetIndex = setIndex;
      this.UpdatePages();
   }

   // Each body's text, in segment order, joined by \x1E (line breaks as \n). Blank page: one body.
   // Undefined once the editor is gone (PrepForClose): there is no text, not an empty one.
   function EditGetBodies()
   {
      if(this.aSegs == undefined || this.EditField == undefined)
      {
         return undefined;
      }
      var out = [];
      var k = 0;
      while(k < this.aSegs.length)
      {
         if(this.aSegs[k].editable)
         {
            out.push(this.EditField.text.substring(this.BodyStart(k), this.BodyEnd(k)).split("\r").join("\n"));
         }
         k++;
      }
      return out.join(String.fromCharCode(30));
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
      // Every segment after the first starts a page, on the line after its locked "\r".
      var nextSeg = 1;
      var nextSegLine = this.aSegs.length > 1 ? this.SegStart(1) + 1 : -1;
      var i = 0;
      while(i < tf.numLines)
      {
         var m = tf.getLineMetrics(i);
         var off = tf.getLineOffset(i);
         var forced = false;
         while(nextSegLine >= 0 && nextSegLine <= off)
         {
            forced = forced || nextSegLine == off;
            nextSeg++;
            nextSegLine = nextSeg < this.aSegs.length ? this.SegStart(nextSeg) + 1 : -1;
         }
         // Same rule as CalculatePagination: a line whose bottom passes the page starts a new
         // page; so does each segment's first line.
         if(i > 0 && (forced || y + m.ascent + m.descent > tops[tops.length - 1] + this.iMaxPageHeight))
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
      pos = this.EditSnap(pos, 1);
      var landed = pos < 0 ? -1 : this.PageOfPos(pos);
      // A book's spread shows p and p + 1; a note one page.
      if(landed == p || !this.bNote && landed == p + 1)
      {
         this.EditSetCaret(pos);
         this.EditLayout();
      }
      else
      {
         // Nowhere to type on that page (the blank or title page): show it, keep the caret.
         this.iEditPage = p;
         this.ShowEditPage(p);
      }
      return true;
   }

   function PageOfPos(pos)
   {
      var tf = this.EditField;
      var line = pos >= tf.length ? tf.numLines - 1 : tf.getLineIndexOfChar(pos);
      var p = 0;
      while(p + 1 < this.aEditPageLines.length && this.aEditPageLines[p + 1] <= line)
      {
         p++;
      }
      return p;
   }


   function EditCaret()
   {
      var pos = Selection.getBeginIndex();
      return pos < 0 ? this.EditField.length : pos;
   }

   function EditSetCaret(pos)
   {
      this.EditField.type = "input";
      this.EditField.selectable = true;
      Selection.setFocus(this.EditField);
      Selection.setSelection(pos, pos);
   }

   // Typing goes into the body at the caret; the caret is only ever in a body.
   function AppendEditChar(ch)
   {
      if(this.EditField == undefined)
      {
         return undefined;
      }
      if(ch == "\n")
      {
         ch = "\r";
      }
      var pos = this.EditSnap(this.EditCaret(), 1);
      var k = this.EditableSegAt(pos);
      if(k < 0)
      {
         return undefined;
      }
      this.EditField.replaceText(pos, pos, ch);
      this.EditField.setTextFormat(pos, pos + ch.length, this.EditField.getNewTextFormat());
      this.aSegs[k].body += ch.length;
      this.FormatBreaks(k);
      this.EditSetCaret(pos + ch.length);
      this.EditLayout();
   }

   // Stops at the start of the body: headings and the title page can't be deleted.
   function EditBackspace()
   {
      if(this.EditField == undefined)
      {
         return undefined;
      }
      var pos = this.EditCaret();
      var k = this.EditableSegAt(pos);
      if(k >= 0 && pos > this.BodyStart(k))
      {
         this.EditField.replaceText(pos - 1, pos, "");
         this.aSegs[k].body -= 1;
         this.FormatBreaks(k);
         this.EditSetCaret(pos - 1);
      }
      this.EditLayout();
   }

   function EditDelete()
   {
      if(this.EditField == undefined)
      {
         return undefined;
      }
      var pos = this.EditCaret();
      var k = this.EditableSegAt(pos);
      if(k >= 0 && pos < this.BodyEnd(k))
      {
         this.EditField.replaceText(pos, pos + 1, "");
         this.aSegs[k].body -= 1;
         this.FormatBreaks(k);
         this.EditSetCaret(pos);
      }
      this.EditLayout();
   }

   // Moves like a text box, then out of any locked text in the direction of travel.
   function EditMoveCursor(direction)
   {
      if(this.EditField == undefined)
      {
         return undefined;
      }
      var tf = this.EditField;
      var pos = this.EditCaret();
      var len = tf.length;
      var target = pos;
      var dir = 1;
      if(direction == "left")
      {
         target = pos - 1;
         dir = -1;
      }
      else if(direction == "right")
      {
         target = pos + 1;
      }
      else if(direction == "home")
      {
         target = 0;
      }
      else if(direction == "end")
      {
         target = len;
         dir = -1;
      }
      else if(direction == "up" || direction == "down")
      {
         var line = pos >= len ? tf.numLines - 1 : tf.getLineIndexOfChar(pos);
         var other = direction == "up" ? line - 1 : line + 1;
         dir = direction == "up" ? -1 : 1;
         if(other >= 0 && other < tf.numLines)
         {
            var col = pos - tf.getLineOffset(line);
            var otherStart = tf.getLineOffset(other);
            var otherEnd = other + 1 < tf.numLines ? tf.getLineOffset(other + 1) - 1 : len;
            target = Math.min(otherStart + col, otherEnd);
         }
      }
      if(target < 0 || target > len)
      {
         target = pos;
      }
      var snapped = this.EditSnap(target, dir);
      this.EditSetCaret(snapped < 0 ? pos : snapped);
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
      this.oEditContent = undefined;
      this.aSegs = undefined;
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

   // DIAGNOSTIC: a field's paragraph format and its first lines' geometry, to compare the
   // reading pages with the editor.
   static function DescribeLayout(tf)
   {
      if(tf == undefined)
      {
         return "layout=undefined";
      }
      var f = tf.getTextFormat(0);
      var out = "layout{fmt0=" + f.font + "/" + f.size + " align=" + f.align + " lm=" + f.leftMargin + " rm=" + f.rightMargin + " indent=" + f.indent + " lead=" + f.leading + " block=" + f.blockIndent + " kern=" + f.kerning + " ls=" + f.letterSpacing + " lines:";
      var i = 0;
      var y = 0;
      while(i < tf.numLines && i < 6)
      {
         var m = tf.getLineMetrics(i);
         var off = tf.getLineOffset(i);
         var lf = tf.getTextFormat(off < tf.length ? off : 0);
         out += " [" + i + " y=" + y + " x=" + m.x + " w=" + Math.round(m.width) + " h=" + m.height + " asc=" + m.ascent + " desc=" + m.descent + " lead=" + m.leading + " size=" + lf.size + " '" + tf.text.substr(off, 12) + "']";
         y += m.height;
         i++;
      }
      return out + "}";
   }

   static function DescribeSegs(segs)
   {
      if(segs == undefined)
      {
         return "none";
      }
      var out = [];
      var k = 0;
      while(k < segs.length)
      {
         out.push((segs[k].editable ? "E" : "L") + segs[k].locked + "+" + segs[k].body);
         k++;
      }
      return segs.length + "[" + out.join(",") + "]";
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
      var s = "note=" + this.bNote + " edit=" + this.bEditMode + " sizes=" + BookMenu.FONT_SIZE_B + "/" + BookMenu.FONT_SIZE_N + " editPage=" + this.iEditPage + " maxPageH=" + this.iMaxPageHeight + " left=" + this.iLeftPageNumber + " set=" + this.iPageSetIndex + " pageInfo=" + this.PageInfoA.length + " pagination=" + this.iPaginationIndex;
      s += " | editPages=" + this.EditPageCount() + " tops=" + this.aEditPageTops.join(",") + " firstLines=" + this.aEditPageLines.join(",") + " shows=" + this.iShowCalls + " segs=" + BookMenu.DescribeSegs(this.aSegs) + " lastShow=" + this.iLastShowOffset + " shownFrom=" + this.iEditShownFrom + " editPage=" + this.iEditPage + " turns=" + this.sTurnLog + (this.EditField == undefined ? "" : " fieldY=" + this.EditField._y + " maskY=" + this.EditMask._y + " maskH=" + this.EditMask._height + " clipVis=" + this.EditClip._visible);
      s += " | stage{w=" + Stage.width + " h=" + Stage.height + " mode=" + Stage.scaleMode + " rect=" + Stage.visibleRect.x + "," + Stage.visibleRect.y + "," + Stage.visibleRect.width + "," + Stage.visibleRect.height + "}";
      s += " | focus=" + Selection.getFocus() + " caret=" + Selection.getBeginIndex();
      s += " | " + BookMenu.DescribeClip("menu", this);
      s += " | " + BookMenu.DescribeClip("ref", this.ReferenceText_mc) + " " + BookMenu.DescribeField("refField", this.ReferenceTextField);
      s += " | refRuns{" + BookMenu.DescribeRuns(this.ReferenceTextField) + "} refHtml{" + String(this.ReferenceTextField.htmlText).substr(0,2000) + "}";
      s += " | " + BookMenu.DescribeClip("edit", this.EditClip) + " " + BookMenu.DescribeField("editField", this.EditField) + " " + BookMenu.DescribeLayout(this.EditField);
      var i = 0;
      while(i < this.BookPages.length)
      {
         s += " | page" + this.BookPages[i].pageNum + ":" + BookMenu.DescribeClip("", this.BookPages[i]) + " " + BookMenu.DescribeField("field", this.BookPages[i].PageTextField) + " " + BookMenu.DescribeLayout(this.BookPages[i].PageTextField);
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
         if(!_loc3_ && this.iPageSetIndex + _loc4_ >= 0 && (this.PageInfoA.length > this.iPageSetIndex + _loc4_ + 1 || this.iPaginationIndex == -1 && this.PageInfoA.length > this.iPageSetIndex + _loc4_))
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
