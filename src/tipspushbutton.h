/*
  tipsradiobutton.h - DM&P 86Scratch
  Copyright (c) 2018 Vic Chen <vic@dmp.com.tw>. All right reserved.
  Copyright (c) 2018 RoBoardGod <roboardgod@dmp.com.tw>. All right reserved.

  This program is free software; you can redistribute it and/or
  modify it under the terms of the GNU General Public License as
  published by the Free Software Foundation; either version 2 of
  the License, or (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program; if not, write to the Free Software
  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston,
  MA  02110-1301  USA

  (If you need a commercial license, please contact soc@dmp.com.tw
   to get more information.)
*/

#ifndef TIPSPUSHBUTTON_H
#define TIPSPUSHBUTTON_H

#include <QPushButton>

class TipsPushButton : public QPushButton
{
    Q_OBJECT
public:
    TipsPushButton(QWidget *parent = 0);

signals:
    void enter();
    void leave();

protected:
    void enterEvent(QEvent *ev);
    void leaveEvent(QEvent *ev);

};

#endif // TIPSPUSHBUTTON_H
