                        /*** /

This file is part of Golly, a Game of Life Simulator.
Copyright (C) 2006 Andrew Trevorrow and Tomas Rokicki.

This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.

 Web site:  http://sourceforge.net/projects/golly
 Authors:   rokicki@gmail.com  andrew@trevorrow.com

                        / ***/

/* -------------------- Some notes on Golly's display code ---------------------

The rectangular area used to display patterns is called the viewport.
It's represented by a window of class PatternView defined in wxview.h.
The global viewptr points to a PatternView window which is created near
the bottom of MainFrame::MainFrame() in wxmain.cpp.

Nearly all drawing in the viewport is done in this module.  The only other
places are in wxview.cpp: PatternView::StartDrawingCells() is called when
the pencil cursor is used to click a cell, and PatternView::DrawCells() is
called while the user drags the pencil cursor around to draw many cells.
These exceptions were done for performance reasons -- using update events
to do the drawing was just too slow.

The main rendering routine is DrawView() -- see the end of this module.
DrawView() is called from PatternView::OnPaint(), the update event handler
for the viewport window.  Update events are generated automatically by the
wxWidgets event dispatcher, or they can be generated manually by calling
MainFrame::UpdateEverything() or MainFrame::UpdatePatternAndStatus().

DrawView() does the following tasks:

- Calls currlayer->algo->draw() to draw the current pattern.  It passes
  in renderer, an instance of wx_render (derived from liferender) which
  has 2 methods: killrect() draws a rectangular area of dead cells,
  and blit() draws a monochrome bitmap with at least one live cell.
  
  Note that currlayer->algo->draw() does all the hard work of figuring
  out which parts of the viewport are dead and building all the bitmaps
  for the live parts.  The bitmaps contain suitably shrunken images
  when the scale is < 1:1 (ie. mag < 0).  At scales 1:2 and above,
  DrawStretchedBitmap() is called from wx_render::blit().
  
  Each lifealgo needs to implement its own draw() method.  Currently
  we have hlifealgo::draw() in hlifedraw.cpp and qlifealgo::draw()
  in qlifedraw.cpp.

- Calls DrawGridLines() to overlay grid lines if they are visible.

- Calls DrawSelection() to overlay a translucent selection rectangle
  if a selection exists and any part of it is visible.

- Calls DrawOtherLayers() to overlay multiple layers using the current
  layer's scale and location.

- If the user is doing a paste, CheckPasteImage() creates a temporary
  viewport (tempview) and draws the paste pattern (stored in pastealgo)
  into a masked bitmap which is then used by DrawPasteImage().

Potential optimizations:

- Every time DrawView() is called it draws the entire viewport, so
  there's plenty of room for improvement!  One fairly simple change
  would be to maintain a list of rects seen by wx_render::killrect()
  and only draw new rects when necessary (the list would need to be
  emptied after various UI changes).  Other more complicated schemes
  that only do incremental drawing might also be worth trying.

- The bitmap data passed into wx_render::blit() is an XBM image where
  the bit order in each byte is reversed.  This is required by the
  wxBitmap constructor when creating monochrome bitmaps.  Presumably
  the bit order needs to be reversed back before it can be used with
  the Mac's native blitter (and Windows?) so it would probably be good
  to avoid the bit reversal and call the native blitter directly.

Other points of interest:

- The decision to use buffered drawing is made in PatternView::OnPaint().
  To avoid flicker, buffering is always used if:
  - the user is doing a paste;
  - the grid lines are visible;
  - the selection is visible;
  - multiple layers are being displayed.

- Change the "#if 0" to "#if 1" in wx_render::killrect() to see randomly
  colored rects.  Gives an insight into how lifealgo::draw() works.

- The viewport window is actually the right-hand pane of a wxSplitWindow.
  The left-hand pane is a directory control (wxGenericDirCtrl) that
  displays the user's preferred pattern or script folder.  This is all
  handled in wxmain.cpp.

----------------------------------------------------------------------------- */

#include "wx/wxprec.h"     // for compilers that support precompilation
#ifndef WX_PRECOMP
   #include "wx/wx.h"      // for all others include the necessary headers
#endif

#include "wx/image.h"      // for wxImage
#include "wx/rawbmp.h"     // for wxAlphaPixelData

#include "bigint.h"
#include "lifealgo.h"
#include "viewport.h"

#include "wxgolly.h"       // for viewptr, statusptr
#include "wxutils.h"       // for Warning, Fatal, FillRect
#include "wxprefs.h"       // for swapcolors, showgridlines, mingridmag, etc
#include "wxstatus.h"      // for statusptr->...
#include "wxview.h"        // for viewptr->...
#include "wxlayer.h"       // currlayer, GetLayer, etc
#include "wxrender.h"

// -----------------------------------------------------------------------------

// global data used in wx_render routines
wxDC* currdc;              // current device context for viewport
int currwd, currht;        // current width and height of viewport
wxBrush* killbrush;        // brush used in killrect

// bitmap memory for drawing magnified cells (see DrawStretchedBitmap)
const int MAGSIZE = 256;
wxUint16 magarray[MAGSIZE * MAGSIZE / 16];
unsigned char* magbuf = (unsigned char *)magarray;

// this lookup table magnifies bits in a given byte by a factor of 2;
// it assumes input and output are in XBM format (bits in each byte are reversed)
// because that's what wxWidgets requires when creating a monochrome bitmap
wxUint16 Magnify2[256];

// for drawing selection image (initialized in InitDrawingData)
#ifdef __WXX11__
   // wxX11's Blit doesn't support alpha channel
#else
   wxImage selimage;       // translucent overlay for drawing selections
   int selimagewd;         // width of selection image
   int selimageht;         // height of selection image
#endif
wxBitmap* selbitmap = NULL;   // selection bitmap (if NULL then inversion is used)

// for drawing paste image (initialized in CreatePasteImage)
wxBitmap* pastebitmap;     // paste bitmap
int pimagewd;              // width of paste image
int pimageht;              // height of paste image
int prectwd;               // must match viewptr->pasterect.width
int prectht;               // must match viewptr->pasterect.height
int pastemag;              // must match current viewport's scale
int cvwd, cvht;            // must match current viewport's width and height
paste_location pasteloc;   // must match plocation
lifealgo* pastealgo;       // universe containing paste pattern
wxRect pastebbox;          // bounding box in cell coords (not necessarily minimal)

// for drawing multiple layers
wxBitmap* layerbitmap = NULL;    // layer bitmap
int layerwd = -1;                // width of layer bitmap
int layerht = -1;                // height of layer bitmap

// -----------------------------------------------------------------------------

// initialize Magnify2 table; note that it swaps byte order if running on
// a little-endian processor
void InitMagnifyTable()
{
   int inttest = 1;
   unsigned char *p = (unsigned char *)&inttest;
   int i;
   for (i=0; i<8; i++)
      if (*p)
         Magnify2[1<<i] = 3 << (2 * i);
      else
         Magnify2[1<<i] = 3 << (2 * (i ^ 4));
   for (i=0; i<256; i++)
      if (i & (i-1))
         Magnify2[i] = Magnify2[i & (i-1)] + Magnify2[i & - i];
}

// -----------------------------------------------------------------------------

void InitDrawingData()
{
   InitMagnifyTable();
   
   #ifdef __WXX11__
      // wxX11's Blit doesn't support alpha channel
   #else
      // create translucent selection image
      if ( !selimage.Create(1,1) ) {
         Fatal(_("Failed to create selection image!"));
      }
      selimage.SetRGB(0, 0, selectrgb->Red(), selectrgb->Green(), selectrgb->Blue());
      selimage.SetAlpha();                   // add alpha channel
      if ( selimage.HasAlpha() ) {
         selimage.SetAlpha(0, 0, 128);       // 50% opaque
      } else {
         Warning(_("Selection image has no alpha channel!"));
      }
      // scale selection image to viewport size and create selbitmap
      int wd, ht;
      viewptr->GetClientSize(&wd, &ht);
      // wd or ht might be < 1 on Win/X11 platforms
      if (wd < 1) wd = 1;
      if (ht < 1) ht = 1;
      selimage.Rescale(wd, ht);
      selimagewd = wd;
      selimageht = ht;
      selbitmap = new wxBitmap(selimage);
      if (selbitmap == NULL) {
         Warning(_("Not enough memory for selection image!"));
      } else {
         #ifdef __WXMAC__
            // bug??? GetDepth returns -1 even if screen depth is really 32
         #else
            // use inversion if depth is < 24
            if (selbitmap->GetDepth() < 24) {
               delete selbitmap;
               selbitmap = NULL;
            }
         #endif
      }
   #endif
}

// -----------------------------------------------------------------------------

void DestroyDrawingData()
{
   #ifdef __WXX11__
      // wxX11 doesn't support alpha channel
   #else
      selimage.Destroy();
      if (selbitmap) delete selbitmap;
   #endif
   
   if (layerbitmap) delete layerbitmap;
}

// -----------------------------------------------------------------------------

// called from wx_render::blit to magnify given bitmap by pmag (2, 4, ... 2^MAX_MAG)
void DrawStretchedBitmap(wxDC &dc, int xoff, int yoff, int *bmdata, int bmsize, int pmag)
{
   int blocksize, magsize, rowshorts, numblocks, numshorts, numbytes;
   int rowbytes = bmsize / 8;
   int row, col;                    // current block
   int xw, yw;                      // window pos of scaled block's top left corner

   // try to process bmdata in square blocks of size MAGSIZE/pmag so each
   // magnified block is MAGSIZE x MAGSIZE
   blocksize = MAGSIZE / pmag;
   magsize = MAGSIZE;
   if (blocksize > bmsize) {
      blocksize = bmsize;
      magsize = bmsize * pmag;      // only use portion of magarray
   }
   rowshorts = magsize / 16;
   numbytes = rowshorts * 2;

   // pmag must be <= numbytes so numshorts (see below) will be > 0
   if (pmag > numbytes) {
      // this should never happen if max pmag is 16 (MAX_MAG = 4) and min bmsize is 64
      Fatal(_("DrawStretchedBitmap cannot magnify by this amount!"));
   }
   
   // nicer to have gaps between cells at scales > 1:2
   wxUint16 gapmask = 0;
   if ( (pmag > 2 && pmag < (1 << mingridmag)) ||
        (pmag >= (1 << mingridmag) && !showgridlines) ) {
      // we use 7/7F rather than E/FE because of XBM bit reversal
      if (pmag == 4) {
         gapmask = 0x7777;
      } else if (pmag == 8) {
         gapmask = 0x7F7F;
      } else if (pmag == 16) {
         unsigned char *p = (unsigned char *)&gapmask;
         gapmask = 0xFF7F;
         // swap byte order if little-endian processor
         if (*p != 0xFF) gapmask = 0x7FFF;
      }
   }

   yw = yoff;
   numblocks = bmsize / blocksize;
   for ( row = 0; row < numblocks; row++ ) {
      xw = xoff;
      for ( col = 0; col < numblocks; col++ ) {
         if ( xw < currwd && xw + magsize >= 0 &&
              yw < currht && yw + magsize >= 0 ) {
            // some part of magnified block will be visible;
            // set bptr to address of byte at top left corner of current block
            unsigned char *bptr = (unsigned char *)bmdata + (row * blocksize * rowbytes) +
                                                            (col * blocksize / 8);
            
            int rowindex = 0;       // first row in magbuf
            int i, j;
            for ( i = 0; i < blocksize; i++ ) {
               // use lookup table to convert bytes in bmdata to 16-bit ints in magbuf
               numshorts = numbytes / pmag;
               for ( j = 0; j < numshorts; j++ ) {
                  magarray[rowindex + j] = Magnify2[ bptr[j] ];
               }
               while (numshorts < rowshorts) {
                  // stretch completed bytes in current row starting from right end
                  unsigned char *p = (unsigned char *)&magarray[rowindex + numshorts];
                  for ( j = numshorts * 2 - 1; j >= 0; j-- ) {
                     p--;
                     magarray[rowindex + j] = Magnify2[ *p ];
                  }
                  numshorts *= 2;
               }
               if (gapmask > 0) {
                  // erase pixel at right edge of each cell
                  for ( j = 0; j < rowshorts; j++ )
                     magarray[rowindex + j] &= gapmask;
                  // duplicate current magbuf row pmag-2 times
                  for ( j = 2; j < pmag; j++ ) {
                     memcpy(&magarray[rowindex + rowshorts], &magarray[rowindex], rowshorts*2);
                     rowindex += rowshorts;
                  }
                  rowindex += rowshorts;
                  // erase pixel at bottom edge of each cell
                  memset(&magarray[rowindex], 0, rowshorts*2);
               } else {
                  // duplicate current magbuf row pmag-1 times
                  for ( j = 1; j < pmag; j++ ) {
                     memcpy(&magarray[rowindex + rowshorts], &magarray[rowindex], rowshorts*2);
                     rowindex += rowshorts;
                  }
               }
               rowindex += rowshorts;     // start of next row in magbuf
               bptr += rowbytes;          // start of next row in current block
            }
            
            wxBitmap magmap = wxBitmap((const char *)magbuf, magsize, magsize, 1);
            dc.DrawBitmap(magmap, xw, yw, false);
         }
         xw += magsize;     // across to next block
      }
      yw += magsize;     // down to next block
   }
}

// -----------------------------------------------------------------------------

class wx_render : public liferender
{
public:
   wx_render() {}
   virtual ~wx_render() {}
   virtual void killrect(int x, int y, int w, int h);
   virtual void blit(int x, int y, int w, int h, int *bm, int bmscale=1);
};

void wx_render::killrect(int x, int y, int w, int h)
{
   if (w <= 0 || h <= 0) return;
   wxRect r(x, y, w, h);
   #if 0
      // use a different pale color each time to see any probs
      wxBrush randbrush(wxColor((rand()&127)+128,
                                (rand()&127)+128,
                                (rand()&127)+128));
      FillRect(*currdc, r, randbrush);
   #else
      FillRect(*currdc, r, *killbrush);
   #endif
}

void wx_render::blit(int x, int y, int w, int h, int *bmdata, int bmscale)
{
   if (bmscale == 1) {
      wxBitmap bmap = wxBitmap((const char *)bmdata, w, h, 1);
      currdc->DrawBitmap(bmap, x, y);
   } else {
      // stretch bitmap by bmscale
      DrawStretchedBitmap(*currdc, x, y, bmdata, w / bmscale, bmscale);
   }
}

wx_render renderer;     // create instance

// -----------------------------------------------------------------------------

void CheckSelectionImage(int viewwd, int viewht)
{
   #ifdef __WXX11__
      // wxX11 doesn't support alpha channel
   #else
      if (selbitmap) {
         if ( viewwd != selimagewd || viewht != selimageht ) {
            // rescale selection image and create new bitmap
            selimage.Rescale(viewwd, viewht);
            delete selbitmap;
            selbitmap = new wxBitmap(selimage);
            // don't check if selbitmap is NULL here (done in DrawSelection)
            selimagewd = viewwd;
            selimageht = viewht;
         }
      }
   #endif
}

// -----------------------------------------------------------------------------

void SetSelectionColor()
{
   #ifdef __WXX11__
      // wxX11 doesn't support alpha channel
   #else
      if (selbitmap) {
         // shrink selection image so we can update color
         selimage.Rescale(1, 1);
         selimage.SetRGB(0, 0, selectrgb->Red(), selectrgb->Green(), selectrgb->Blue());
         // restore image size and create new bitmap
         selimage.Rescale(selimagewd, selimageht);
         delete selbitmap;
         selbitmap = new wxBitmap(selimage);
         // don't check if selbitmap is NULL here (done in DrawSelection)
      }
   #endif
}

// -----------------------------------------------------------------------------

void DrawSelection(wxDC &dc, wxRect &rect)
{
   if (selbitmap) {
      // draw translucent rect
      #ifdef __WXGTK__
         // wxGTK Blit doesn't support alpha channel
         dc.DrawBitmap(selbitmap->GetSubBitmap(rect), rect.x, rect.y, true);
      #else
         // Blit seems to be about 10% faster (on Mac at least)
         wxMemoryDC memdc;
         memdc.SelectObject(*selbitmap);
         dc.Blit(rect.x, rect.y, rect.width, rect.height, &memdc, 0, 0, wxCOPY, true);
      #endif
   } else {
      // no alpha channel so just invert rect
      dc.Blit(rect.x, rect.y, rect.width, rect.height, &dc, rect.x, rect.y, wxINVERT);
   }
}

// -----------------------------------------------------------------------------

void CreatePasteImage(lifealgo *palgo, wxRect &bbox)
{
   pastealgo = palgo;      // save for use in CheckPasteImage
   pastebbox = bbox;       // ditto
   pastebitmap = NULL;
   prectwd = -1;           // force CheckPasteImage to update paste image
   prectht = -1;
   pimagewd = -1;          // force CheckPasteImage to rescale paste image
   pimageht = -1;
   pastemag = viewptr->GetMag();
}

// -----------------------------------------------------------------------------

void DestroyPasteImage()
{
   if (pastebitmap) {
      delete pastebitmap;
      pastebitmap = NULL;
   }
}

// -----------------------------------------------------------------------------

int PixelsToCells(int pixels) {
   // convert given # of screen pixels to corresponding # of cells
   if (pastemag >= 0) {
      int cellsize = 1 << pastemag;
      return (pixels + cellsize - 1) / cellsize;
   } else {
      // pastemag < 0; no need to worry about overflow
      return pixels << -pastemag;
   }
}

// -----------------------------------------------------------------------------

void CheckPasteImage()
{
   // paste image needs to be updated if pasterect size changed
   // or viewport size changed or cell colors changed or plocation changed
   if ( prectwd != viewptr->pasterect.width ||
        prectht != viewptr->pasterect.height ||
        cvwd != currlayer->view->getwidth() ||
        cvht != currlayer->view->getheight() ||
        pasteloc != plocation
      ) {
      prectwd = viewptr->pasterect.width;
      prectht = viewptr->pasterect.height;
      pastemag = currlayer->view->getmag();
      cvwd = currlayer->view->getwidth();
      cvht = currlayer->view->getheight();
      pasteloc = plocation;

      // calculate size of paste image; we could just set it to pasterect size
      // but that would be slow and wasteful for large pasterects, so we use
      // the following code (the only tricky bit is when plocation = Middle)
      int pastewd = prectwd;
      int pasteht = prectht;
      
      if (pastewd <= 2 || pasteht <= 2) {
         // no need to draw paste image because border lines will cover it
         if (pastebitmap) delete pastebitmap;
         pastebitmap = NULL;
         if (pimagewd != 1 || pimageht != 1) {
            pimagewd = 1;
            pimageht = 1;
         }
         return;
      }
      
      wxRect cellbox = pastebbox;
      if (pastewd > currlayer->view->getwidth() || pasteht > currlayer->view->getheight()) {
         if (plocation == Middle) {
            // temporary viewport may need to be TWICE size of current viewport
            if (pastewd > 2 * currlayer->view->getwidth())
               pastewd = 2 * currlayer->view->getwidth();
            if (pasteht > 2 * currlayer->view->getheight())
               pasteht = 2 * currlayer->view->getheight();
            if (pastemag > 0) {
               // make sure pastewd/ht don't have partial cells
               int cellsize = 1 << pastemag;
               if ((pastewd + 1) % cellsize > 0)
                  pastewd += cellsize - ((pastewd + 1) % cellsize);
               if ((pasteht + 1) % cellsize != 0)
                  pasteht += cellsize - ((pasteht + 1) % cellsize);
            }
            if (prectwd > pastewd) {
               // make sure prectwd - pastewd is an even number of *cells*
               // to simplify shifting in DrawPasteImage
               if (pastemag > 0) {
                  int cellsize = 1 << pastemag;
                  int celldiff = (prectwd - pastewd) / cellsize;
                  if (celldiff & 1) pastewd += cellsize;
               } else {
                  if ((prectwd - pastewd) & 1) pastewd++;
               }
            }
            if (prectht > pasteht) {
               // make sure prectht - pasteht is an even number of *cells*
               // to simplify shifting in DrawPasteImage
               if (pastemag > 0) {
                  int cellsize = 1 << pastemag;
                  int celldiff = (prectht - pasteht) / cellsize;
                  if (celldiff & 1) pasteht += cellsize;
               } else {
                  if ((prectht - pasteht) & 1) pasteht++;
               }
            }
         } else {
            // plocation is at a corner of pasterect so temporary viewport
            // may need to be size of current viewport
            if (pastewd > currlayer->view->getwidth())
               pastewd = currlayer->view->getwidth();
            if (pasteht > currlayer->view->getheight())
               pasteht = currlayer->view->getheight();
            if (pastemag > 0) {
               // make sure pastewd/ht don't have partial cells
               int cellsize = 1 << pastemag;
               int gap = 1;                        // gap between cells
               if (pastemag == 1) gap = 0;         // no gap at scale 1:2
               if ((pastewd + gap) % cellsize > 0)
                  pastewd += cellsize - ((pastewd + gap) % cellsize);
               if ((pasteht + gap) % cellsize != 0)
                  pasteht += cellsize - ((pasteht + gap) % cellsize);
            }
            cellbox.width = PixelsToCells(pastewd);
            cellbox.height = PixelsToCells(pasteht);
            if (plocation == TopLeft) {
               // show top left corner of pasterect
               cellbox.x = pastebbox.x;
               cellbox.y = pastebbox.y;
            } else if (plocation == TopRight) {
               // show top right corner of pasterect
               cellbox.x = pastebbox.x + pastebbox.width - cellbox.width;
               cellbox.y = pastebbox.y;
            } else if (plocation == BottomRight) {
               // show bottom right corner of pasterect
               cellbox.x = pastebbox.x + pastebbox.width - cellbox.width;
               cellbox.y = pastebbox.y + pastebbox.height - cellbox.height;
            } else { // plocation == BottomLeft
               // show bottom left corner of pasterect
               cellbox.x = pastebbox.x;
               cellbox.y = pastebbox.y + pastebbox.height - cellbox.height;
            }
         }
      }
      
      // delete old bitmap and corresponding mask, even if size hasn't changed
      if (pastebitmap) delete pastebitmap;
      pimagewd = pastewd;
      pimageht = pasteht;
      // create new bitmap (-1 means screen depth)
      pastebitmap = new wxBitmap(pimagewd, pimageht, -1);
      // mask will be created after drawing pattern into bitmap
      
      if (pastebitmap) {
         // create temporary viewport and draw pattern into pastebitmap
         // for later use in DrawPasteImage
         viewport tempview(pimagewd, pimageht);
         int midx, midy;
         if (pastemag > 0) {
            midx = cellbox.x + cellbox.width / 2;
            midy = cellbox.y + cellbox.height / 2;
         } else {
            midx = cellbox.x + (cellbox.width - 1) / 2;
            midy = cellbox.y + (cellbox.height - 1) / 2;
         }
         tempview.setpositionmag(midx, midy, pastemag);
         
         wxMemoryDC pattdc;
         pattdc.SelectObject(*pastebitmap);
         
         // set foreground and background colors for DrawBitmap calls
         #if (defined(__WXMAC__) && !wxCHECK_VERSION(2,7,2)) || defined(__WXMSW__)
            // use opposite meaning on Mac/Windows -- sheesh
            pattdc.SetTextForeground(*deadrgb);
            pattdc.SetTextBackground(*pastergb);
         #else
            pattdc.SetTextForeground(*pastergb);
            pattdc.SetTextBackground(*deadrgb);
         #endif

         // set brush color used in killrect
         killbrush = deadbrush;
         
         // temporarily turn off grid lines for DrawStretchedBitmap
         bool saveshow = showgridlines;
         showgridlines = false;
         
         currdc = &pattdc;
         currwd = tempview.getwidth();
         currht = tempview.getheight();
         pastealgo->draw(tempview, renderer);
         
         showgridlines = saveshow;

         // add mask to pastebitmap
         #ifdef __WXMSW__
            // temporarily change depth to avoid bug in wxMSW
            int d = pastebitmap->GetDepth();
            pastebitmap->SetDepth(1);
            pastebitmap->SetMask( new wxMask(*pastebitmap,*deadrgb) );
            pastebitmap->SetDepth(d);
         #else
            pastebitmap->SetMask( new wxMask(*pastebitmap,*deadrgb) );
         #endif

      } else {
         // give some indication that pastebitmap could not be created
         wxBell();
      }
   }
}

// -----------------------------------------------------------------------------

void DrawPasteImage(wxDC &dc)
{
   if (pastebitmap) {
      // draw paste image
      wxMemoryDC memdc;
      memdc.SelectObject(*pastebitmap);
      wxRect r = viewptr->pasterect;
      if (r.width > pimagewd || r.height > pimageht) {
         // paste image is smaller than pasterect (which can't fit in viewport)
         // so shift image depending on plocation
         switch (plocation) {
            case TopLeft:
               // no need to do any shifting
               break;
            case TopRight:
               // shift image to top right corner of pasterect
               r.x += r.width - pimagewd;
               break;
            case BottomRight:
               // shift image to bottom right corner of pasterect
               r.x += r.width - pimagewd;
               r.y += r.height - pimageht;
               break;
            case BottomLeft:
               // shift image to bottom left corner of pasterect
               r.y += r.height - pimageht;
               break;
            case Middle:
               // shift image to middle of pasterect; note that CheckPasteImage
               // has ensured (r.width - pimagewd) and (r.height - pimageht)
               // are an even number of *cells* if pastemag > 0
               r.x += (r.width - pimagewd) / 2;
               r.y += (r.height - pimageht) / 2;
               break;
         }
      }
      dc.Blit(r.x, r.y, pimagewd, pimageht, &memdc, 0, 0, wxCOPY, true);
   }

   // now overlay border rectangle
   dc.SetPen(*pastepen);
   dc.SetBrush(*wxTRANSPARENT_BRUSH);

   // if large rect then we need to avoid overflow because DrawRectangle
   // has problems on Mac if given a size that exceeds 32K
   wxRect r = viewptr->pasterect;
   if (r.x < 0) { int diff = -1 - r.x;  r.x = -1;  r.width -= diff; }
   if (r.y < 0) { int diff = -1 - r.y;  r.y = -1;  r.height -= diff; }
   if (r.width > currlayer->view->getwidth())
      r.width = currlayer->view->getwidth() + 2;
   if (r.height > currlayer->view->getheight())
      r.height = currlayer->view->getheight() + 2;
   dc.DrawRectangle(r);
   
   if (r.y > 0) {
      dc.SetFont(*statusptr->GetStatusFont());
      //dc.SetBackgroundMode(wxSOLID);
      //dc.SetTextBackground(*wxWHITE);
      dc.SetBackgroundMode(wxTRANSPARENT);   // better in case pastergb is white
      dc.SetTextForeground(*pastergb);
      wxString pmodestr = wxString(GetPasteMode(),wxConvLocal);
      int pmodex = r.x + 2;
      int pmodey = r.y - 4;
      dc.DrawText(pmodestr, pmodex, pmodey - statusptr->GetTextAscent());
   }
   
   dc.SetBrush(wxNullBrush);
   dc.SetPen(wxNullPen);
}

// -----------------------------------------------------------------------------

void DrawGridLines(wxDC &dc, wxRect &r)
{
   int cellsize = 1 << currlayer->view->getmag();
   int h, v, i, topbold, leftbold;

   if (showboldlines) {
      // ensure that origin cell stays next to bold lines;
      // ie. bold lines scroll when pattern is scrolled
      pair<bigint, bigint> lefttop = currlayer->view->at(0, 0);
      leftbold = lefttop.first.mod_smallint(boldspacing);
      topbold = lefttop.second.mod_smallint(boldspacing);
      if (currlayer->originx != bigint::zero) {
         leftbold -= currlayer->originx.mod_smallint(boldspacing);
      }
      if (currlayer->originy != bigint::zero) {
         topbold -= currlayer->originy.mod_smallint(boldspacing);
      }
      if (mathcoords) topbold--;   // show origin cell above bold line
   } else {
      // avoid spurious gcc warning
      topbold = leftbold = 0;
   }

   // draw all plain lines first
   dc.SetPen(swapcolors ? *sgridpen : *gridpen);
   i = showboldlines ? topbold : 1;
   v = -1;
   while (true) {
      v += cellsize;
      if (v >= r.height) break;
      if (showboldlines) i++;
      if (i % boldspacing != 0 && v >= r.y && v < r.y + r.height)
         dc.DrawLine(r.x, v, r.GetRight() + 1, v);
   }
   i = showboldlines ? leftbold : 1;
   h = -1;
   while (true) {
      h += cellsize;
      if (h >= r.width) break;
      if (showboldlines) i++;
      if (i % boldspacing != 0 && h >= r.x && h < r.x + r.width)
         dc.DrawLine(h, r.y, h, r.GetBottom() + 1);
   }

   if (showboldlines) {
      // overlay bold lines
      dc.SetPen(swapcolors ? *sboldpen : *boldpen);
      i = topbold;
      v = -1;
      while (true) {
         v += cellsize;
         if (v >= r.height) break;
         i++;
         if (i % boldspacing == 0 && v >= r.y && v < r.y + r.height)
            dc.DrawLine(r.x, v, r.GetRight() + 1, v);
      }
      i = leftbold;
      h = -1;
      while (true) {
         h += cellsize;
         if (h >= r.width) break;
         i++;
         if (i % boldspacing == 0 && h >= r.x && h < r.x + r.width)
            dc.DrawLine(h, r.y, h, r.GetBottom() + 1);
      }
   }
   
   dc.SetPen(wxNullPen);
}

// -----------------------------------------------------------------------------

void DrawOneLayer(wxDC &dc, int index, viewport *thisview)
{
   wxMemoryDC layerdc;
   layerdc.SelectObject(*layerbitmap);
   
   // set foreground and background colors for DrawBitmap calls
   #if (defined(__WXMAC__) && !wxCHECK_VERSION(2,7,2)) || defined(__WXMSW__)
      // use opposite meaning on Mac/Windows -- sheesh
      layerdc.SetTextForeground(*deadrgb);
      layerdc.SetTextBackground(*livergb[index]);
   #else
      layerdc.SetTextForeground(*livergb[index]);
      layerdc.SetTextBackground(*deadrgb);
   #endif
   
   currdc = &layerdc;
   currlayer->algo->draw(*thisview, renderer);
   
   // access pixel data and make all dead pixels 100% transparent
   // and all live pixels slightly transparent
   wxAlphaPixelData data(*layerbitmap, wxPoint(0,0), wxSize(layerwd,layerht));
   if (data) {
      const int livealpha = 192;    // make live pixels 75% opaque???
      //!!! let user adjust opacity???
   
      int deadr = deadrgb->Red();
      int deadg = deadrgb->Green();
      int deadb = deadrgb->Blue();

      data.UseAlpha();
      wxAlphaPixelData::Iterator p(data);
      for ( int y = 0; y < layerht; y++ ) {
         wxAlphaPixelData::Iterator rowStart = p;
         for ( int x = 0; x < layerwd; x++ ) {
            // get pixel color
            int r = p.Red();
            int g = p.Green();
            int b = p.Blue();

            // set alpha value depending on whether pixel is live or dead
            if (r == deadr && g == deadg && b == deadb) {
               // make dead pixel 100% transparent
               p.Red()   = 0;
               p.Green() = 0;
               p.Blue()  = 0;
               p.Alpha() = 0;
            } else {
               // live pixel; note that RGB must be premultiplied by alpha
               p.Red()   = r * livealpha / 256;
               p.Green() = g * livealpha / 256;
               p.Blue()  = b * livealpha / 256;
               p.Alpha() = livealpha;
            }

            p++;
         }
         p = rowStart;
         p.OffsetY(data, 1);
      }
      
      // draw result
      dc.DrawBitmap(*layerbitmap, 0, 0, true);

   } else {
      // could not access pixel data so use slower masking code
      // with no alpha transparency

      // add mask to layerbitmap
      #ifdef __WXMSW__
         // temporarily change depth to avoid bug in wxMSW
         int d = layerbitmap->GetDepth();
         layerbitmap->SetDepth(1);
         layerbitmap->SetMask( new wxMask(*layerbitmap,*deadrgb) );
         layerbitmap->SetDepth(d);
      #else
         layerbitmap->SetMask( new wxMask(*layerbitmap,*deadrgb) );
      #endif
   
      // draw masked image
      layerdc.SelectObject(*layerbitmap);
      dc.Blit(0, 0, layerwd, layerht, &layerdc, 0, 0, wxCOPY, true);
   }
}

// -----------------------------------------------------------------------------

void DrawOtherLayers(wxDC &dc)
{
   // check if layerbitmap needs to be created or resized
   if ( layerwd != currlayer->view->getwidth() ||
        layerht != currlayer->view->getheight() ) {
      layerwd = currlayer->view->getwidth();
      layerht = currlayer->view->getheight();
      if (layerbitmap) delete layerbitmap;
      // create a bitmap of depth 32 so it has room for alpha channel
      layerbitmap = new wxBitmap(layerwd, layerht, 32);
      if (!layerbitmap) {
         Warning(_("Not enough memory for layer bitmap!"));
         return;
      }
   }

   // temporarily turn off grid lines for DrawStretchedBitmap
   bool saveshow = showgridlines;
   showgridlines = false;

   // set brush color used in killrect
   killbrush = deadbrush;
   
   // draw patterns in layers 1..numlayers-1
   for ( int i = 1; i < numlayers; i++ ) {
      Layer *savelayer = currlayer;
      currlayer = GetLayer(i);

      if ( !currlayer->algo->isEmpty() ) {
         // draw layer using the real current layer's viewport
         DrawOneLayer(dc, i, savelayer->view);
      }
      
      // draw this layer's selection if necessary
      wxRect r;
      if ( viewptr->SelectionVisible(&r) ) {
         CheckSelectionImage(layerwd, layerht);
         DrawSelection(dc, r);
      }
      
      // restore currlayer
      currlayer = savelayer;
   }
   
   showgridlines = saveshow;
}

// -----------------------------------------------------------------------------

void DrawView(wxDC &dc)
{
   wxRect r;
   Layer *savelayer = NULL;
   viewport *saveview0 = NULL;
   int colorindex;
   
   if ( numlayers > 1 && drawlayers ) {
      // draw all layers starting with layer 0 but using current layer's viewport
      savelayer = currlayer;
      if ( currindex != 0 ) {
         viewport *currentview = currlayer->view;
         // change currlayer to layer 0
         currlayer = GetLayer(0);
         saveview0 = currlayer->view;
         currlayer->view = currentview;
      }
      colorindex = 0;
   } else {
      // just draw the current layer
      colorindex = currindex;
   }

   if ( viewptr->nopattupdate ) {
      // don't draw incomplete pattern, just fill background
      r = wxRect(0, 0, currlayer->view->getwidth(), currlayer->view->getheight());
      FillRect(dc, r, swapcolors ? *livebrush[colorindex] : *deadbrush);
   } else {
      // set foreground and background colors for DrawBitmap calls
      #if (defined(__WXMAC__) && !wxCHECK_VERSION(2,7,2)) || defined(__WXMSW__)
      // use opposite meaning on Mac/Windows -- sheesh
      if ( swapcolors ) {
      #else
      if ( !swapcolors ) {
      #endif
         dc.SetTextForeground(*livergb[colorindex]);
         dc.SetTextBackground(*deadrgb);
      } else {
         dc.SetTextForeground(*deadrgb);
         dc.SetTextBackground(*livergb[colorindex]);
      }
      // set brush color used in killrect
      killbrush = swapcolors ? livebrush[colorindex] : deadbrush;
      // draw pattern using a sequence of blit and killrect calls
      currdc = &dc;
      currwd = currlayer->view->getwidth();
      currht = currlayer->view->getheight();
      currlayer->algo->draw(*currlayer->view, renderer);
   }

   if ( viewptr->GridVisible() ) {
      r = wxRect(0, 0, currlayer->view->getwidth(), currlayer->view->getheight());
      DrawGridLines(dc, r);
   }
   
   if ( viewptr->SelectionVisible(&r) ) {
      CheckSelectionImage(currlayer->view->getwidth(), currlayer->view->getheight());
      DrawSelection(dc, r);
   }
   
   if ( savelayer ) {
      // must restore currlayer before we call DrawOtherLayers
      currlayer = savelayer;
      if ( saveview0 ) {
         // restore layer 0's viewport
         GetLayer(0)->view = saveview0;
      }
      if ( !viewptr->nopattupdate ) {
         // draw layers 1, 2, ... numlayers-1
         DrawOtherLayers(dc);
      }
   }
   
   if ( viewptr->waitingforclick && viewptr->pasterect.width > 0 ) {
      // this test is not really necessary, but it avoids unnecessary
      // drawing of the paste image when user changes scale
      if ( pastemag != viewptr->GetMag() &&
           prectwd == viewptr->pasterect.width && prectwd > 1 &&
           prectht == viewptr->pasterect.height && prectht > 1 ) {
         // don't draw old paste image, a new one is coming very soon
      } else {
         CheckPasteImage();
         DrawPasteImage(dc);
      }
   }
}
