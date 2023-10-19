//-----------------------------------------------------------------------------
// Copyright (C) Proxmark3 contributors. See AUTHORS.md for details.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// See LICENSE.txt for the text of the license.
//-----------------------------------------------------------------------------
// GUI (QT)
//-----------------------------------------------------------------------------
#define __STDC_FORMAT_MACROS
#include "proxguiqt.h"
#include <inttypes.h>
#include <stdbool.h>
#include <iostream>
//#include <QtCore>
#include <QPainterPath>
#include <QBrush>
#include <QPen>
#include <QTimer>
#include <QCloseEvent>
#include <QMouseEvent>
#include <QKeyEvent>
#include <math.h>
#include <limits.h>
#include <stdio.h>
#include <array>
#include <vector>
#include <algorithm>
#include <QSlider>
#include <QHBoxLayout>
#include <string.h>
#include <QtGui>
#include "proxgui.h"
#include "ui.h"
#include "comms.h"
#include "graph.h"
#include "cmddata.h"
#include "cmdlf.h"
#include "util_darwin.h"
//#include "fileutils.h"
#include <bits/stdc++.h>
extern "C" int preferences_save(void);

using namespace std;
template<typename D>
int maxElement(vector<D> const &v) {
    int max = INT_MIN;
    for (const D &i: v) {
        if (max < i){
            max = i;
        }
    }
    return max;
}

template<typename D>
int minElement(vector<D> const &v){
    int min = INT_MAX;
    for (const D &i: v){
        if (min > i){
            min = i;
        }
    }
    return min;
}

static int s_Buff[MAX_GRAPH_TRACE_LEN];
static bool gs_useOverlays = false;
static int gs_absVMax = 0;
static uint32_t startMax; // Maximum offset in the graph (right side of graph)
static uint32_t PageWidth; // How many samples are currently visible on this 'page' / graph
static int unlockStart = 0;

static int half_distance = 0;
static int tmp_b = 0;
static int tmp_a = 0;
static int silence_start = 0;
static int silence_stop = 0;
static int amplitude_start = 0;
static int amplitude_stop = 0;
static int half_init = 0;
static int half_end = 0;
static int cut_start = 0;
static int cut_end = 0;
static int x_start = 0;
static int x_stop = 0;
static int delta_x = 0;
static int third_distance_frag = 0;
static int third_distance_start = 0;
static int third_distance_stop = 0;
static int strtidx = 0;
static int clip_frags = 0;

auto min_sample = 0;
auto max_sample = 0;
vector<int> samples;
bool show_both = false;
bool big_zoom_set = false;
bool remove_zero = true;
bool show_sliders = false;
static int orig_buff[MAX_GRAPH_TRACE_LEN];
static int cut_buff[MAX_GRAPH_TRACE_LEN];
static int cut_buff_idx = 0;
bool first_run = true;
bool cursor_pool = false;
bool norm_cascade = false;
bool demod_space = false;
bool saved_demod = false;

static int A_before = 0;
static int B_before = 0;
static int C_before = 0;
static int D_before = 0;

void ProxGuiQT::ShowGraphWindow(void) {
    emit ShowGraphWindowSignal();
}

void ProxGuiQT::RepaintGraphWindow(void) {
    emit RepaintGraphWindowSignal();
}

void ProxGuiQT::HideGraphWindow(void) {
    emit HideGraphWindowSignal();
}

// emit picture viewer signals
void ProxGuiQT::ShowPictureWindow(const QImage &img) {
    emit ShowPictureWindowSignal(img);
}

void ProxGuiQT::ShowBase64PictureWindow(char *b64) {
    emit ShowBase64PictureWindowSignal(b64);
}

void ProxGuiQT::RepaintPictureWindow(void) {
    emit RepaintPictureWindowSignal();
}

void ProxGuiQT::HidePictureWindow(void) {
    emit HidePictureWindowSignal();
}

void ProxGuiQT::Exit(void) {
    emit ExitSignal();
}

void ProxGuiQT::_ShowGraphWindow(void) {
    if (!plotapp)
        return;

    if (!plotwidget) {

#if defined(__MACH__) && defined(__APPLE__)
        makeFocusable();
#endif

        plotwidget = new ProxWidget();
    }
    plotwidget->show();

}

void ProxGuiQT::_RepaintGraphWindow(void) {
    if (!plotapp || !plotwidget)
        return;

    plotwidget->update();
}

void ProxGuiQT::_HideGraphWindow(void) {
    if (!plotapp || !plotwidget)
        return;

    plotwidget->hide();
}

// picture viewer
void ProxGuiQT::_ShowPictureWindow(const QImage &img) {

    if (!plotapp)
        return;

    if (img.isNull())
        return;

    if (!pictureWidget) {

#if defined(__MACH__) && defined(__APPLE__)
        makeFocusable();
#endif

        pictureWidget = new PictureWidget();
    }

    QPixmap pm = QPixmap::fromImage(img);

    //QPixmap newPixmap = pm.scaled(QSize(50,50),  Qt::KeepAspectRatio);
    //pm = pm.scaled(pictureController->lbl_pm->size(),  Qt::KeepAspectRatio);

    pictureController->lbl_pm->setPixmap(pm);
    pictureController->lbl_pm->setScaledContents(false);
    pictureController->lbl_pm->setAlignment(Qt::AlignCenter);

    QString s = QString("w: %1  h: %2")
                .arg(pm.size().width())
                .arg(pm.size().height()
                    );
    pictureController->lbl_sz->setText(s);
    pictureWidget->show();
}

void ProxGuiQT::_ShowBase64PictureWindow(char *b64) {

    if (!plotapp)
        return;

    if (b64 == NULL)
        return;

    size_t slen = strlen(b64);
    if (slen == 0)
        return;

    char *myb64data = (char *)calloc(slen + 1, sizeof(uint8_t));
    if (myb64data == NULL)
        return;

    memcpy(myb64data, b64, slen);

    if (!pictureWidget) {

#if defined(__MACH__) && defined(__APPLE__)
        makeFocusable();
#endif

        pictureWidget = new PictureWidget();
    }

    QPixmap pm;
    if (pm.loadFromData(QByteArray::fromBase64(myb64data), "PNG") == false) {
        qWarning("Failed to read base64 data: %s", myb64data);
    }
    free(myb64data);
    //free(b64);

    pictureController->lbl_pm->setPixmap(pm);
    pictureController->lbl_pm->setScaledContents(false);
    pictureController->lbl_pm->setAlignment(Qt::AlignCenter);

    QString s = QString("w: %1  h: %2")
                .arg(pm.size().width())
                .arg(pm.size().height()
                    );
    pictureController->lbl_sz->setText(s);
    pictureWidget->show();

}

void ProxGuiQT::_RepaintPictureWindow(void) {
    if (!plotapp || !pictureWidget)
        return;

    pictureWidget->update();
}

void ProxGuiQT::_HidePictureWindow(void) {
    if (!plotapp || !pictureWidget)
        return;

    pictureWidget->hide();
}

void ProxGuiQT::_Exit(void) {
    delete this;
}

void ProxGuiQT::_StartProxmarkThread(void) {
    if (!proxmarkThread)
        return;

    // if thread finished delete self and delete application
    QObject::connect(proxmarkThread, SIGNAL(finished()), proxmarkThread, SLOT(deleteLater()));
    QObject::connect(proxmarkThread, SIGNAL(finished()), this, SLOT(_Exit()));
    // start proxmark thread
    proxmarkThread->start();
}

void ProxGuiQT::MainLoop() {
    plotapp = new QApplication(argc, argv);

    // Setup the picture widget
    pictureWidget = new PictureWidget();
    pictureController = new Ui::PictureForm();
    pictureController->setupUi(pictureWidget);
//    pictureWidget->setAttribute(Qt::WA_DeleteOnClose,true);

    // Set picture widget position if no settings.
    if (g_session.preferences_loaded == false) {
        // Move controller widget below plot
        //pictureController->move(x(), y() + frameSize().height());
        //pictureController->resize(size().width(), 200);
    }

    connect(this, SIGNAL(ShowGraphWindowSignal()), this, SLOT(_ShowGraphWindow()));
    connect(this, SIGNAL(RepaintGraphWindowSignal()), this, SLOT(_RepaintGraphWindow()));
    connect(this, SIGNAL(HideGraphWindowSignal()), this, SLOT(_HideGraphWindow()));

    connect(this, SIGNAL(ExitSignal()), this, SLOT(_Exit()));

    // hook up picture viewer signals
    connect(this, SIGNAL(ShowPictureWindowSignal(const QImage &)), this, SLOT(_ShowPictureWindow(const QImage &)));
    connect(this, SIGNAL(ShowBase64PictureWindowSignal(char *)), this, SLOT(_ShowBase64PictureWindow(char *)));
    connect(this, SIGNAL(RepaintPictureWindowSignal()), this, SLOT(_RepaintPictureWindow()));
    connect(this, SIGNAL(HidePictureWindowSignal()), this, SLOT(_HidePictureWindow()));

    //start proxmark thread after starting event loop
    QTimer::singleShot(200, this, SLOT(_StartProxmarkThread()));

#if defined(__MACH__) && defined(__APPLE__)
    //Prevent the terminal from loosing focus during launch by making the client unfocusable
    makeUnfocusable();
#endif

    //orig_buff = g_GraphBuffer;
    //memcpy(orig_buff, g_GraphBuffer, g_GraphTraceLen * sizeof(uint32_t));
    plotapp->exec();
}

ProxGuiQT::ProxGuiQT(int argc, char **argv, WorkerThread *wthread) :
    plotapp(NULL), plotwidget(NULL), pictureController(NULL), pictureWidget(NULL), argc(argc), argv(argv), proxmarkThread(wthread) {

}

ProxGuiQT::~ProxGuiQT(void) {

    if (pictureController) {
        delete pictureController;
        pictureController = NULL;
    }

    if (pictureWidget) {
        pictureWidget->close();
        delete pictureWidget;
        pictureWidget = NULL;
    }

    if (plotapp) {
        plotapp->quit();
        plotapp = NULL;
    }
}

// -------------------------------------------------
// Slider Widget form based on a class to enable
// Event override functions
// -------------------------------------------------
PictureWidget::PictureWidget() {
    // Set the initial position and size from settings
//    if (g_session.preferences_loaded)
//        setGeometry(g_session.pw.x, g_session.pw.y, g_session.pw.w, g_session.pw.h);
//    else
    resize(900, 700);
}

void PictureWidget::closeEvent(QCloseEvent *event) {
    this->hide();
    event->ignore();
}


// -------------------------------------------------
// Slider Widget form based on a class to enable
// Event override functions
// -------------------------------------------------

SliderWidget::SliderWidget() {
    // Set the initial position and size from settings
    if (g_session.preferences_loaded)
        setGeometry(g_session.overlay.x, g_session.overlay.y, g_session.overlay.w, g_session.overlay.h);
    else
        resize(800, 400);
}

void SliderWidget::resizeEvent(QResizeEvent *event) {
    g_session.overlay.h = event->size().height();
    g_session.overlay.w = event->size().width();
    g_session.window_changed = true;

}

void SliderWidget::moveEvent(QMoveEvent *event) {
    g_session.overlay.x = event->pos().x();
    g_session.overlay.y = event->pos().y();
    g_session.window_changed = true;
}

//--------------------
void ProxWidget::applyOperation() {
    //printf("ApplyOperation()");
    save_restoreGB(GRAPH_SAVE);
    memcpy(g_GraphBuffer, s_Buff, sizeof(int) * g_GraphTraceLen);
    RepaintGraphWindow();
}
void ProxWidget::stickOperation() {
    save_restoreGB(GRAPH_RESTORE);
    //printf("stickOperation()");
}
void ProxWidget::vchange_autocorr(int v) {
    int ans = AutoCorrelate(g_GraphBuffer, s_Buff, g_GraphTraceLen, v, true, false);
    if (g_debugMode) printf("vchange_autocorr(w:%d): %d\n", v, ans);
    gs_useOverlays = true;
    RepaintGraphWindow();
}
void ProxWidget::vchange_askedge(int v) {
    //extern int AskEdgeDetect(const int *in, int *out, int len, int threshold);
    int ans = AskEdgeDetect(g_GraphBuffer, s_Buff, g_GraphTraceLen, v);
    if (g_debugMode) printf("vchange_askedge(w:%d)%d\n", v, ans);
    gs_useOverlays = true;
    RepaintGraphWindow();
}
void ProxWidget::vchange_dthr_up(int v) {
    int down = opsController->horizontalSlider_dirthr_down->value();
    directionalThreshold(g_GraphBuffer, s_Buff, g_GraphTraceLen, v, down);
    //printf("vchange_dthr_up(%d)", v);
    gs_useOverlays = true;
    RepaintGraphWindow();
}
void ProxWidget::vchange_dthr_down(int v) {
    //printf("vchange_dthr_down(%d)", v);
    int up = opsController->horizontalSlider_dirthr_up->value();
    directionalThreshold(g_GraphBuffer, s_Buff, g_GraphTraceLen, v, up);
    gs_useOverlays = true;
    RepaintGraphWindow();
}


ProxWidget::ProxWidget(QWidget *parent, ProxGuiQT *master) : QWidget(parent) {
    this->master = master;
    // Set the initial position and size from settings
    if (g_session.preferences_loaded)
        setGeometry(g_session.plot.x, g_session.plot.y, g_session.plot.w, g_session.plot.h);
    else
        resize(800, 400);

    // Setup the controller widget
    controlWidget = new SliderWidget();
    opsController = new Ui::Form();
    opsController->setupUi(controlWidget);
    //Due to quirks in QT Designer, we need to fiddle a bit
    opsController->horizontalSlider_dirthr_down->setMinimum(-128);
    opsController->horizontalSlider_dirthr_down->setMaximum(0);
    opsController->horizontalSlider_dirthr_down->setValue(-20);
    opsController->horizontalSlider_dirthr_up->setMinimum(-40);
    opsController->horizontalSlider_dirthr_up->setMaximum(128);
    opsController->horizontalSlider_dirthr_up->setValue(20);
    opsController->horizontalSlider_askedge->setValue(25);
    opsController->horizontalSlider_window->setValue(4000);

    QObject::connect(opsController->pushButton_apply, SIGNAL(clicked()), this, SLOT(applyOperation()));
    QObject::connect(opsController->pushButton_sticky, SIGNAL(clicked()), this, SLOT(stickOperation()));
    QObject::connect(opsController->horizontalSlider_window, SIGNAL(valueChanged(int)), this, SLOT(vchange_autocorr(int)));
    QObject::connect(opsController->horizontalSlider_dirthr_up, SIGNAL(valueChanged(int)), this, SLOT(vchange_dthr_up(int)));
    QObject::connect(opsController->horizontalSlider_dirthr_down, SIGNAL(valueChanged(int)), this, SLOT(vchange_dthr_down(int)));
    QObject::connect(opsController->horizontalSlider_askedge, SIGNAL(valueChanged(int)), this, SLOT(vchange_askedge(int)));

    controlWidget->setGeometry(g_session.overlay.x, g_session.overlay.y, g_session.overlay.w, g_session.overlay.h);

    // Set up the plot widget, which does the actual plotting
    plot = new Plot(this);
    QVBoxLayout *layout = new QVBoxLayout;
    layout->addWidget(plot);
    setLayout(layout);

    // plot window title
    QString pt = QString("- - - - - Plot [ %1 ] - - - - -").arg(g_conn.serial_port_name);
    setWindowTitle(pt);

    // shows plot window on the screen.
    show();

    // Set Slider/Overlay position if no settings.
    if (g_session.preferences_loaded == false) {
        // Move controller widget below plot
        controlWidget->move(x(), y() + frameSize().height());
        controlWidget->resize(size().width(), 200);
    }

    // Overlays / slider window title
    QString ct = QString("- - - - - Slider [ %1 ] - - - - -").arg(g_conn.serial_port_name);
    controlWidget->setWindowTitle(ct);

    // The hide/show event functions should take care of this.
    //  controlWidget->show();

    // now that is up, reset pos/size change flags
    g_session.window_changed = false;

}

// not 100% sure what i need in this block
// feel free to fix - marshmellow...
ProxWidget::~ProxWidget(void) {
    if (controlWidget) {
        controlWidget->close();
        delete controlWidget;
        controlWidget = NULL;
    }

    if (opsController) {
        delete opsController;
        opsController = NULL;
    }

    if (plot) {
        plot->close();
        delete plot;
        plot = NULL;
    }
}

void ProxWidget::closeEvent(QCloseEvent *event) {
    event->ignore();
    this->hide();
    gs_useOverlays = false;
}
void ProxWidget::hideEvent(QHideEvent *event) {
    controlWidget->hide();
    plot->hide();
}
void ProxWidget::showEvent(QShowEvent *event) {
    if (g_session.overlay_sliders)
        controlWidget->show();
    else
        controlWidget->hide();

    plot->show();
}
void ProxWidget::moveEvent(QMoveEvent *event) {
    g_session.plot.x = event->pos().x();
    g_session.plot.y = event->pos().y();
    g_session.window_changed = true;
}
void ProxWidget::resizeEvent(QResizeEvent *event) {
    g_session.plot.h = event->size().height();
    g_session.plot.w = event->size().width();
    g_session.window_changed = true;
}

//----------- Plotting

int Plot::xCoordOf(int i, QRect r) {
    return r.left() + (int)((i - g_GraphStart) * g_GraphPixelsPerPoint);
}

int Plot::yCoordOf(int v, QRect r, int maxVal) {
    int z = (r.bottom() - r.top()) / 2;
    if (maxVal == 0) ++maxVal;
    return -(z * v) / maxVal + z;
}

int Plot::valueOf_yCoord(int y, QRect r, int maxVal) {
    int z = (r.bottom() - r.top()) / 2;
    return (y - z) * maxVal / z;
}

static const QColor BLACK     = QColor(0, 0, 0);
static const QColor GRAY60    = QColor(60, 60, 60);
static const QColor GRAY100   = QColor(100, 100, 100);
static const QColor GRAY240   = QColor(240, 240, 240);
static const QColor WHITE     = QColor(255, 255, 255);
static const QColor GREEN     = QColor(100, 255, 100);
static const QColor RED       = QColor(255, 100, 100);
static const QColor BLUE      = QColor(100, 100, 255);
static const QColor YELLOW    = QColor(255, 255, 0);
static const QColor PINK      = QColor(255, 0, 255);
static const QColor ORANGE    = QColor(255, 153, 0);
static const QColor LIGHTBLUE = QColor(100, 209, 246);

QColor Plot::getColor(int graphNum) {
    switch (graphNum) {
        case 0:
            return LIGHTBLUE;
        case 1:
            return RED;
        case 2:
            return BLUE;
        default:
            return GRAY240;
    }
}

void Plot::setMaxAndStart(int *buffer, size_t len, QRect plotRect) {
    if (len == 0) return;
    startMax = 0;
    if (plotRect.right() >= plotRect.left() + 40) {
        uint32_t t = (plotRect.right() - plotRect.left() - 40) / g_GraphPixelsPerPoint;
        if (len >= t)
            startMax = len - t;
    }
    if (g_GraphStart > startMax) {
        g_GraphStart = startMax;
    }
    if (g_GraphStart > len) return;
    int vMin = INT_MAX, vMax = INT_MIN;
    uint32_t sample_index = g_GraphStart ;
    for (; sample_index < len && xCoordOf(sample_index, plotRect) < plotRect.right() ; sample_index++) {

        int v = buffer[sample_index];
        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
    }

    gs_absVMax = 0;
    if (fabs((double) vMin) > gs_absVMax) gs_absVMax = (int)fabs((double) vMin);
    if (fabs((double) vMax) > gs_absVMax) gs_absVMax = (int)fabs((double) vMax);
    gs_absVMax = (int)(gs_absVMax * 1.25 + 1);
}

void Plot::PlotDemod(uint8_t *buffer, size_t len, QRect plotRect, QRect annotationRect, QPainter *painter, int graphNum, uint32_t plotOffset) {
    if (len == 0 || g_PlotGridX <= 0) return;
    //clock_t begin = clock();
    QPainterPath penPath;

    int grid_delta_x = g_PlotGridX;
    int first_delta_x = grid_delta_x; //(plotOffset > 0) ? g_PlotGridX : (g_PlotGridX +);
    if (g_GraphStart > plotOffset) first_delta_x -= (g_GraphStart - plotOffset);
    int DemodStart = g_GraphStart;
    if (plotOffset > g_GraphStart) DemodStart = plotOffset;

    int BitStart = 0;
    // round down
    if (DemodStart - plotOffset > 0) BitStart = (int)(((DemodStart - plotOffset) + (g_PlotGridX - 1)) / g_PlotGridX) - 1;
    first_delta_x += BitStart * g_PlotGridX;
    if (BitStart > (int)len) return;
//    int v = 0;
    //printf("first_delta_x %i, grid_delta_x %i, DemodStart %i, BitStart %i\n",first_delta_x,grid_delta_x,DemodStart, BitStart);

    painter->setPen(BLUE);
    char str[5];
    int absVMax = (int)(100 * 1.05 + 1);
    delta_x = 0;
    int clk = first_delta_x;
    for (int i = BitStart; i < (int)len && xCoordOf(delta_x + DemodStart, plotRect) < plotRect.right(); i++) {
        for (int j = 0; j < (clk) && i < (int)len && xCoordOf(DemodStart + delta_x + j, plotRect) < plotRect.right() ; j++) {
            int x = xCoordOf(DemodStart + delta_x + j, plotRect);
            int v = buffer[i] * 200 - 100;

            int y = yCoordOf(v, plotRect, absVMax);
            if ((i == BitStart) && (j == 0)) { // First point
                penPath.moveTo(x, y);
            } else {
                penPath.lineTo(x, y);
            }
            if (g_GraphPixelsPerPoint > 10) {
                QRect f(QPoint(x - 3, y - 3), QPoint(x + 3, y + 3));
                painter->fillRect(f, getColor(graphNum));
            }
            if (j == (int)clk / 2) {
                //print label
                snprintf(str, sizeof(str), "%u", buffer[i]);
                painter->drawText(x - 8, y + ((buffer[i] > 0) ? 18 : -6), str);
            }
        }
        delta_x += clk;
        clk = grid_delta_x;
    }

    // Graph annotations
    painter->drawPath(penPath);
}

void Plot::PlotGraph(int *buffer, size_t len, QRect plotRect, QRect annotationRect, QPainter *painter, int graphNum) {
    if(first_run){
        copy_n(g_GraphBuffer, MAX_GRAPH_TRACE_LEN, orig_buff);
        first_run = false;
    }
    if (len == 0) return;
    // clock_t begin = clock();
    QPainterPath penPath;
    int vMin = INT_MAX, vMax = INT_MIN, v = 0;
    int64_t vMean = 0;
    uint32_t i = 0;
    int x = xCoordOf(g_GraphStart, plotRect);
    int y = yCoordOf(buffer[g_GraphStart], plotRect, gs_absVMax);
    penPath.moveTo(x, y);
    for (i = g_GraphStart; i < len && xCoordOf(i, plotRect) < plotRect.right(); i++) {

        x = xCoordOf(i, plotRect);
        v = buffer[i];

        y = yCoordOf(v, plotRect, gs_absVMax);

        penPath.lineTo(x, y);

        if (g_GraphPixelsPerPoint > 10) {
            QRect f(QPoint(x - 3, y - 3), QPoint(x + 3, y + 3));
            painter->fillRect(f, GREEN);
        }
        // catch stats
        if (v < vMin) vMin = v;
        if (v > vMax) vMax = v;
        vMean += v;
    }
    g_GraphStop = i;
    vMean /= (g_GraphStop - g_GraphStart);

    painter->setPen(getColor(graphNum));

    // Draw y-axis
    int xo = 5 + (graphNum * 40);
    painter->drawLine(xo, plotRect.top(), xo, plotRect.bottom());

    int vMarkers = (gs_absVMax - (gs_absVMax % 10)) / 5;
    int minYDist = 20; // Minimum pixel-distance between markers

    char yLbl[20];

    int n = 0;
    int lasty0 = 65535;

    for (v = vMarkers; yCoordOf(v, plotRect, gs_absVMax) > plotRect.top() && n < 20; v += vMarkers, n++) {
        int y0 = yCoordOf(v, plotRect, gs_absVMax);
        int y1 = yCoordOf(-v, plotRect, gs_absVMax);

        if (lasty0 - y0 < minYDist) continue;

        painter->drawLine(xo - 5, y0, xo + 5, y0);

        snprintf(yLbl, sizeof(yLbl), "%d", v);
        painter->drawText(xo + 8, y0 + 7, yLbl);

        painter->drawLine(xo - 5, y1, xo + 5, y1);
        snprintf(yLbl, sizeof(yLbl), "%d", -v);
        painter->drawText(xo + 8, y1 + 5, yLbl);
        lasty0 = y0;
    }

    //Graph annotations
    painter->drawPath(penPath);
    painter->setPen(LIGHTBLUE);
    char str[200];
    snprintf(str, sizeof(str), "⌃%d  ⌄%d  ⌇%" PRId64 "     〈%u/%zu〉     A[%d:%d]  B[%d:%d]  C[%d:%d]  D[%d:%d]",
             vMax, vMin, vMean, g_GraphStop - g_GraphStart, len,
             CursorAPos, buffer[CursorAPos], CursorBPos, buffer[CursorBPos],
             g_CursorCPos, buffer[g_CursorCPos], g_CursorDPos, buffer[g_CursorDPos]);
    painter->drawText(16, annotationRect.bottom() - 10 - 20 * graphNum, str);
    //clock_t end = clock();
    //double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
    //printf("Plot time %f\n", elapsed_secs);
}

void Plot::plotGridLines(QPainter *painter, QRect r) {

    // set g_GridOffset
    if (g_PlotGridX <= 0) return;

    double offset = g_GridOffset;
    if (g_GridLocked && g_PlotGridX) {
        offset = g_GridOffset + g_PlotGridX - fmod(g_GraphStart, g_PlotGridX);
    } else if (!g_GridLocked && g_GraphStart > 0 && g_PlotGridX) {
        offset = g_PlotGridX - fmod(g_GraphStart - offset, g_PlotGridX) + g_GraphStart - unlockStart;
    }
    offset = fmod(offset, g_PlotGridX);
    if (offset < 0) offset += g_PlotGridX;

    double i;
    double grid_delta_x = g_PlotGridX * g_GraphPixelsPerPoint;
    int grid_delta_y = g_PlotGridY;

    if ((g_PlotGridX > 0) && ((g_PlotGridX * g_GraphPixelsPerPoint) > 1)) {
        for (i = (offset * g_GraphPixelsPerPoint); i < r.right(); i += grid_delta_x) {
            painter->drawLine(r.left() + i, r.top(), r.left() + i, r.bottom());
        }
    }

    if (g_PlotGridY > 0) {
        for (i = 0; yCoordOf(i, r, gs_absVMax) > r.top(); i += grid_delta_y) {
            // line above mid
            painter->drawLine(r.left(), yCoordOf(i, r, gs_absVMax), r.right(), yCoordOf(i, r, gs_absVMax));
            // line below mid
            painter->drawLine(r.left(), yCoordOf(-i, r, gs_absVMax), r.right(), yCoordOf(-i, r, gs_absVMax));
        }
    }
}

#define HEIGHT_INFO 70
#define WIDTH_AXES 80

void Plot::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    QBrush brush(GREEN);
    QPen pen(GREEN);

    painter.setFont(QFont("Times New Roman", 13));

    if (CursorAPos > g_GraphTraceLen)
        CursorAPos = 0;
    if (CursorBPos > g_GraphTraceLen)
        CursorBPos = 0;
    if (g_CursorCPos > g_GraphTraceLen)
        g_CursorCPos = 0;
    if (g_CursorDPos > g_GraphTraceLen)
        g_CursorDPos = 0;

    QRect plotRect(WIDTH_AXES, 0, width() - WIDTH_AXES, height() - HEIGHT_INFO);
    QRect infoRect(0, height() - HEIGHT_INFO, width(), HEIGHT_INFO);
    PageWidth = plotRect.width() / g_GraphPixelsPerPoint;

    //Grey background
    painter.fillRect(rect(), BLACK);
    //Black foreground
    painter.fillRect(plotRect, BLACK);

    //init graph variables
    setMaxAndStart(g_GraphBuffer, g_GraphTraceLen, plotRect);

    // center line
    int zeroHeight = plotRect.top() + (plotRect.bottom() - plotRect.top()) / 2;
    painter.setPen(GRAY100);
    painter.drawLine(plotRect.left(), zeroHeight, plotRect.right(), zeroHeight);
    // plot X and Y grid lines
    if(show_both){
        plotGridLines(&painter, plotRect);
    }

    //Start painting graph
    PlotGraph(g_GraphBuffer, g_GraphTraceLen, plotRect, infoRect, &painter, 0);
    if (g_DemodBufferLen > 8) {
        PlotDemod(g_DemodBuffer, g_DemodBufferLen, plotRect, infoRect, &painter, 2, g_DemodStartIdx);
    }
    if (gs_useOverlays) {
        //init graph variables
        setMaxAndStart(s_Buff, g_GraphTraceLen, plotRect);
        PlotGraph(s_Buff, g_GraphTraceLen, plotRect, infoRect, &painter, 1);
    }
    // End graph drawing

    //Draw the cursors
    if (CursorAPos > g_GraphStart && xCoordOf(CursorAPos, plotRect) < plotRect.right()) {
        painter.setPen(GREEN);
        painter.drawLine(xCoordOf(CursorAPos, plotRect), plotRect.top(), xCoordOf(CursorAPos, plotRect), plotRect.bottom());
    }
    if (CursorBPos > g_GraphStart && xCoordOf(CursorBPos, plotRect) < plotRect.right()) {
        painter.setPen(RED);
        painter.drawLine(xCoordOf(CursorBPos, plotRect), plotRect.top(), xCoordOf(CursorBPos, plotRect), plotRect.bottom());
    }
    if (g_CursorCPos > g_GraphStart && xCoordOf(g_CursorCPos, plotRect) < plotRect.right()) {
        painter.setPen(PINK);
        painter.drawLine(xCoordOf(g_CursorCPos, plotRect), plotRect.top(), xCoordOf(g_CursorCPos, plotRect), plotRect.bottom());
    }
    if (g_CursorDPos > g_GraphStart && xCoordOf(g_CursorDPos, plotRect) < plotRect.right()) {
        painter.setPen(YELLOW);
        painter.drawLine(xCoordOf(g_CursorDPos, plotRect), plotRect.top(), xCoordOf(g_CursorDPos, plotRect), plotRect.bottom());
    }

    //Draw annotations
    char str[7000];
    char scalestr[30] = {0};
    if (g_CursorScaleFactor != 1) {
        if (g_CursorScaleFactorUnit[0] == '\x00') {
            snprintf(scalestr, sizeof(scalestr), " [%2.2f] ", ((int32_t)(CursorBPos - CursorAPos)) / g_CursorScaleFactor);
        } else {
            snprintf(scalestr, sizeof(scalestr), " [%2.2f %s] ", ((int32_t)(CursorBPos - CursorAPos)) / g_CursorScaleFactor, g_CursorScaleFactorUnit);
        }
    }
    snprintf(str, sizeof(str), "         ⎌ [%u..%u]               ⎶ [%i]              ⎚ [%2.2f]              ⎘ [%d]                n⏥ [%d]                    ⎍ [X:%d  Y:%d  Xo:%d]",
             g_GraphStart,
             g_GraphStop,
             CursorBPos - CursorAPos,
             g_GraphPixelsPerPoint,
             cut_buff_idx,
             clip_frags,
             (int)g_PlotGridXdefault,
             (int)g_PlotGridYdefault,
             (int)g_GridOffset
            );
    painter.setPen(ORANGE);
    painter.drawText(620, infoRect.bottom() - 49, str);
    char str2[7000];
    snprintf(str2, sizeof(str2), "⁅ %s  %s  %s  %s  %s%s ⁆",
             g_GridLocked ? "⏇" : "⏆",
             cursor_pool ? "⏀" : "⏃",
             norm_cascade ? "⌥" : "⍢",
             demod_space ? "⍜" : "⍛",
             saved_demod ? "⌬" : "⎔",
             scalestr
    );
    painter.setPen(PINK);
    painter.setFont(QFont("Times New Roman", 21));
    painter.drawText(1380, infoRect.bottom() - 12, str2);
    //painter.drawText(940, infoRect.bottom() - 972, str2);
}

Plot::Plot(QWidget *parent) : QWidget(parent), g_GraphPixelsPerPoint(1) {
    //Need to set this, otherwise we don't receive keypress events
    setFocusPolicy(Qt::StrongFocus);
    resize(400, 200);

    QPalette palette(QColor(0, 0, 0, 0));
    palette.setColor(QPalette::WindowText, BLUE);
    palette.setColor(QPalette::Text, WHITE);
    palette.setColor(QPalette::Button, GRAY100);
    setPalette(palette);
    setAutoFillBackground(true);

    CursorAPos = 0;
    CursorBPos = 0;
    g_GraphStart = 0;
    g_GraphStop = 0;

    setWindowTitle(tr("Sliders"));
    master = parent;
}

void Plot::closeEvent(QCloseEvent *event) {
    event->ignore();
    this->hide();
    gs_useOverlays = false;
}

void Plot::Zoom(double factor, uint32_t refX) {
    if (factor >= 1) { // Zoom in
        if (g_GraphPixelsPerPoint <= 25 * factor) {
            g_GraphPixelsPerPoint *= factor;
            if (refX > g_GraphStart) {
                g_GraphStart += (refX - g_GraphStart) - ((refX - g_GraphStart) / factor);
            }
        }
    } else {          // Zoom out
        if (g_GraphPixelsPerPoint >= 0.01 / factor) {
            g_GraphPixelsPerPoint *= factor;
            if (refX > g_GraphStart) {
                if (g_GraphStart >= ((refX - g_GraphStart) / factor) - (refX - g_GraphStart)) {
                    g_GraphStart -= ((refX - g_GraphStart) / factor) - (refX - g_GraphStart);
                } else {
                    g_GraphStart = 0;
                }
            }
        }
    }
}

void Plot::Move(int offset) {
    if (g_GraphTraceLen == 0) return;
    if (offset > 0) { // Move right
        if (g_GraphPixelsPerPoint < 20) {
            g_GraphStart += offset;
        } else {
            g_GraphStart++;
        }
    } else { // Move left
        if (g_GraphPixelsPerPoint < 20) {
            if (g_GraphStart >= (uint) - offset) {
                g_GraphStart += offset;
            } else {
                g_GraphStart = 0;
            }
        } else {
            if (g_GraphStart > 0) {
                g_GraphStart--;
            }
        }
    }
}

void Plot::Trim(void) {
    uint32_t lref, rref;
    const double zoom_offset = 1.148698354997035; // 2**(1/5)
    if ((CursorAPos == 0) || (CursorBPos == 0)) { // if we don't have both cursors set
        lref = g_GraphStart;
        rref = g_GraphStop;
        if (CursorAPos >= lref) {
            CursorAPos -= lref;
        } else {
            CursorAPos = 0;
        }
        if (CursorBPos >= lref) {
            CursorBPos -= lref;
        } else {
            CursorBPos = 0;
        }
    } else {
        lref = CursorAPos < CursorBPos ? CursorAPos : CursorBPos;
        rref = CursorAPos < CursorBPos ? CursorBPos : CursorAPos;
        // g_GraphPixelsPerPoint mush remain a power of zoom_offset
        double GPPPtarget = g_GraphPixelsPerPoint * (g_GraphStop - g_GraphStart) / (rref - lref);
        while (g_GraphPixelsPerPoint < GPPPtarget) {
            g_GraphPixelsPerPoint *= zoom_offset;
        }
        g_GraphPixelsPerPoint /= zoom_offset;
        CursorAPos -= lref;
        CursorBPos -= lref;
    }
    g_DemodStartIdx -= lref;
    for (uint32_t i = lref; i < rref; ++i)
        g_GraphBuffer[i - lref] = g_GraphBuffer[i];
    g_GraphTraceLen = rref - lref;
    g_GraphStart = 0;
}

void Plot::wheelEvent(QWheelEvent *event) {
    // event->delta()
    //  120 => shift right 5%
    // -120 => shift left 5%
    const float move_offset = 0.05;
    // -120+shift => zoom in  (5 times = *2)
    //  120+shift => zoom out (5 times = /2)
    const double zoom_offset = 1.148698354997035; // 2**(1/5)
    if (event->modifiers() & Qt::ShiftModifier) {
// event->position doesn't exist in QT5.12.8, both exist in 5.14.2 and event->x doesn't exist in 5.15.0
#if QT_VERSION >= 0x050d00
        uint32_t x = event->position().x();
#else
        uint32_t x = event->x();
#endif
        x -= WIDTH_AXES;
        x = (int)(x / g_GraphPixelsPerPoint);
        x += g_GraphStart;
// event->angleDelta doesn't exist in QT4, both exist in 5.12.8 and 5.14.2 and event->delta doesn't exist in 5.15.0
#if QT_VERSION >= 0x050d00
        float delta = event->angleDelta().y();
#else
        float delta = event->delta();
#endif
        if (delta < 0) {
            Zoom(zoom_offset, x);
        } else {
            Zoom(1.0 / zoom_offset, x);
        }
    } else {
#if QT_VERSION >= 0x050d00
        Move(PageWidth * (-(float)event->angleDelta().y() / (120 / move_offset)));
#else
        Move(PageWidth * (-(float)event->delta() / (120 / move_offset)));
#endif
    }
    this->update();
}

void Plot::mouseMoveEvent(QMouseEvent *event) {
    int x = event->x();
    x -= WIDTH_AXES;
    x = (int)(x / g_GraphPixelsPerPoint);
    x += g_GraphStart;
    if ((event->buttons() & Qt::LeftButton)) {
        if (cursor_pool){
            g_CursorCPos = x;
        } else {
            CursorAPos = x;
        }
    } else if (event->buttons() & Qt::RightButton) {
        if (cursor_pool){
            g_CursorDPos = x;
        } else {
            CursorBPos = x;
        }
    }
    this->update();
}

void Plot::keyPressEvent(QKeyEvent *event) {
    uint32_t offset; // Left/right movement offset (in sample size)
    const double zoom_offset = 1.148698354997035; // 2**(1/5)
    if (event->modifiers() & Qt::ShiftModifier) {
        if (g_PlotGridX)
            offset = PageWidth - fmod(PageWidth, g_PlotGridX);
        else
            offset = PageWidth;
    } else {
        if (event->modifiers() & Qt::ControlModifier)
            offset = 1;
        else
            offset = (int)(20 / g_GraphPixelsPerPoint);
    }

    switch (event->key()) {
        case Qt::Key_M:
            g_CursorCPos = CursorAPos;
            g_CursorDPos = CursorBPos;
            CursorAPos = 0;
            CursorBPos = 0;
            break;
        case Qt::Key_B:
            CursorAPos = g_CursorCPos;
            CursorBPos = g_CursorDPos;
            break;
        case Qt::Key_C:
            clip_frags += 1;
            cut_buff_idx = 0;
            x_start = CursorAPos;
            x_stop = CursorBPos;
            if (CursorAPos > CursorBPos){
                x_start = CursorBPos;
                x_stop = CursorAPos;
            }
            for (int i = x_start; i < x_stop; i++){
                cut_buff[cut_buff_idx] = g_GraphBuffer[i];
                cut_buff_idx++;
            }
            break;
        case Qt::Key_V:
            strtidx = 0;
            for(uint32_t i = CursorBPos; i < CursorBPos + cut_buff_idx; i++){
                g_GraphBuffer[i] = cut_buff[strtidx];
                strtidx++;
            }
            RepaintGraphWindow();
            break;
        case Qt::Key_9:
            copy_n(orig_buff, MAX_GRAPH_TRACE_LEN, g_GraphBuffer);
            saved_demod = false;
            g_DemodBufferLen = 0;
            break;
        case Qt::Key_F:
            if(show_both){
                show_both = false;
            } else {
                show_both = true;
            }
            RepaintGraphWindow();
            break;
        case Qt::Key_3:
            if (CursorAPos > CursorBPos){
                half_init = CursorBPos;
                half_end = CursorAPos;
            } else {
                half_init = CursorAPos;
                half_end = CursorBPos;
            }
            half_distance = ((half_end - half_init) / 2) + half_init;
            third_distance_frag = ((half_distance - half_init) / 3);
            third_distance_start = half_init + third_distance_frag;
            third_distance_stop = half_init + third_distance_frag * 2;
            g_CursorCPos = third_distance_start;
            g_CursorDPos = third_distance_stop;
            break;
        case Qt::Key_E:
            saved_demod = true;
            tmp_a = CursorAPos;
            tmp_b = CursorBPos;
            if (demod_space){
                CursorAPos = g_DemodStartIdx;
                CursorBPos = g_DemodStartIdx + delta_x;
            } else {
                CursorAPos = g_DemodStartIdx - delta_x;
                CursorBPos = g_DemodStartIdx + delta_x * 2;
            }
            Trim();
            CursorAPos = tmp_a;
            CursorBPos = tmp_b;
            break;
        case Qt::Key_W:
            tmp_a = CursorAPos;
            tmp_b = CursorBPos;
            cut_end = g_GraphTraceLen;
            for (size_t i = 0; i <= g_GraphTraceLen; i++){
                if (g_GraphBuffer[i] == 0){
                    cut_start += 1;
                } else {
                    break;
                }
            }
            for (size_t i = g_GraphTraceLen; i >= 0; i--){
                if (g_GraphBuffer[i] == 0){
                    cut_end -= 1;
                } else {
                    break;
                }
            }
            CursorAPos = cut_start;
            CursorBPos = cut_end;
            Trim();
            CursorAPos = tmp_a;
            CursorBPos = tmp_b;
            break;
        case Qt::Key_O:
            CursorAPos = 0;
            CursorBPos = g_GraphTraceLen;
            break;
        case Qt::Key_4:
            if(show_sliders){
                g_session.overlay_sliders = true;
                HideGraphWindow();
                ShowGraphWindow();
                show_sliders = false;
            } else {
                g_session.overlay_sliders = false;
                HideGraphWindow();
                ShowGraphWindow();
                show_sliders = true;
            }
            break;
        case Qt::Key_5:
            try_detect_modulation();
            break;
        case Qt::Key_6:
            CmdAskEdgeDetect("-t 20");
            break;
        case Qt::Key_7:
            if(cursor_pool){
                cursor_pool = false;
            } else {
                cursor_pool = true;
            }
            break;
        case Qt::Key_K:
            if(norm_cascade){
                norm_cascade = false;
            } else {
                norm_cascade = true;
            }
            break;
        case Qt::Key_D:
            if(demod_space){
                demod_space = false;
            } else {
                demod_space = true;
            }
            break;
        case Qt::Key_N:
            if(!norm_cascade){
                if(remove_zero){
                    CmdNorm("");
                    remove_zero = false;
                } else {
                    CmdHpf("");
                    remove_zero = true;
                }
            } else {

            }
            break;
        case Qt::Key_0:
            CursorAPos = g_GraphStart;
            CursorBPos = g_GraphStop;
            break;
        case Qt::Key_Z:
            if(big_zoom_set){
                Zoom(0.2, CursorAPos);
                big_zoom_set = false;
            } else {
                Zoom(7, CursorAPos);
                big_zoom_set = true;
            }
            break;
        case Qt::Key_P:
            if(big_zoom_set){
                Zoom(0.2, CursorBPos);
                big_zoom_set = false;
            } else {
                Zoom(9, CursorBPos);
                big_zoom_set = true;
            }
            break;
        case Qt::Key_A:
            amplitude_start = CursorAPos;
            amplitude_stop = CursorBPos;
            if (CursorAPos > CursorBPos){
                amplitude_start = CursorBPos;
                amplitude_stop = CursorAPos;
            }
            for (int i = amplitude_start; i < amplitude_stop; i++){
                samples.push_back(g_GraphBuffer[i]);
            }
            min_sample = minElement(samples);
            max_sample = maxElement(samples);
            g_CursorCPos = min_sample;
            g_CursorDPos = max_sample;
            RepaintGraphWindow();
            break;
        case Qt::Key_X:
            silence_start = CursorAPos;
            silence_stop = CursorBPos;
            if (CursorAPos > CursorBPos){
                silence_start = CursorBPos;
                silence_stop = CursorAPos;
            }
            for (int i = silence_start; i < silence_stop; i++){
                g_GraphBuffer[i] = 0;
            }
            break;
        case Qt::Key_2:
            if (CursorAPos > CursorBPos){
                half_init = CursorBPos;
                half_end = CursorAPos;
            } else {
                half_init = CursorAPos;
                half_end = CursorBPos;
            }
            half_distance = ((half_end - half_init) / 2) + half_init;
            g_CursorCPos = half_distance;
            break;
        case Qt::Key_R:
            if((CursorAPos == 0) && (CursorBPos == 0) && (g_CursorCPos == 0) && (g_CursorDPos == 0)){
                CursorAPos = A_before;
                CursorBPos = B_before;
                g_CursorCPos = C_before;
                g_CursorDPos = D_before;
            } else {
                A_before = CursorAPos;
                B_before = CursorBPos;
                C_before = g_CursorCPos;
                D_before = g_CursorDPos;
                CursorAPos = 0;
                CursorBPos = CursorAPos;
                g_CursorCPos = CursorAPos;
                g_CursorDPos = CursorAPos;
            }
            break;
        case Qt::Key_S:
            tmp_b = CursorBPos;
            CursorBPos = CursorAPos;
            CursorAPos = tmp_b;
            break;
        case Qt::Key_Down:
            if (event->modifiers() & Qt::ShiftModifier) {
                if (event->modifiers() & Qt::ControlModifier) {
                    Zoom(zoom_offset, CursorBPos);
                } else {
                    Zoom(2, CursorBPos);
                }
            } else {
                if (event->modifiers() & Qt::ControlModifier) {
                    Zoom(zoom_offset, CursorAPos);
                } else {
                    Zoom(2, CursorAPos);
                }
            }
            break;

        case Qt::Key_Up:
            if (event->modifiers() & Qt::ShiftModifier) {
                if (event->modifiers() & Qt::ControlModifier) {
                    Zoom(1.0 / zoom_offset, CursorBPos);
                } else {
                    Zoom(0.5, CursorBPos);
                }
            } else {
                if (event->modifiers() & Qt::ControlModifier) {
                    Zoom(1.0 / zoom_offset, CursorAPos);
                } else {
                    Zoom(0.5, CursorAPos);
                }
            }
            break;

        case Qt::Key_Right:
            Move(offset);
            break;

        case Qt::Key_Left:
            Move(-offset);
            break;

        case Qt::Key_Greater:
            g_DemodStartIdx += 1;
            break;

        case Qt::Key_Less:
            g_DemodStartIdx -= 1;
            break;

        case Qt::Key_Q:
            if(saved_demod){
                saved_demod = false;
            } else {
                saved_demod = true;
            }
            break;

        case Qt::Key_G:
            if (g_PlotGridX || g_PlotGridY) {
                g_PlotGridX = 0;
                g_PlotGridY = 0;
            } else {
                if (g_PlotGridXdefault < 0)
                    g_PlotGridXdefault = 64;
                if (g_PlotGridYdefault < 0)
                    g_PlotGridYdefault = 0;

                g_PlotGridX = g_PlotGridXdefault;
                g_PlotGridY = g_PlotGridYdefault;
            }
            break;

        case Qt::Key_H: {
            uint8_t old_printAndLog = g_printAndLog;
            g_printAndLog &= PRINTANDLOG_PRINT;
            PrintAndLogEx(NORMAL, "\n\n\t" _GREEN_(" ♥ PLOTTAH ver 1.0 ♥ "));
            PrintAndLogEx(NORMAL, "\n\n" _RED_("⍙  Move:"));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("Home") "/" _MAGENTA_("End"), "Move to the start/end of the graph");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _BLUE_("Mouse wheel"), "Move left/right");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("Left") "/" _MAGENTA_("Right"), "Move left/right");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, " + " _MAGENTA_("Ctrl"), "... by 1 sample");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, " + " _MAGENTA_("Shift"), "... by 1 window");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("PgUp") "/" _MAGENTA_("PgDown"), "Move left/right by 1 window");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⊕  Zoom:"));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("Shift") " + " _BLUE_("Mouse wheel"), "Zoom in/out around mouse cursor");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("Down") "/" _MAGENTA_("Up"), "Zoom in/out around BLUE cursor");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, " + " _MAGENTA_("Ctrl"), "... with smaller increment");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, " + " _MAGENTA_("Shift"), "... around purple cursor");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("z"), "Strong zoom onto A marker");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("p"), "Strong zoom onto B marker");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⁝ Trim:"));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("t"), "Trim data on window or on cursors if defined");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("v"), "Trim silence from start/end");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⊞  Grid and demod:"));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9 + 9, _MAGENTA_("<") "/" _MAGENTA_(">"), "Move demodulation left/right relative to samples");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⍉  Misc"));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("LMB"), "Set BLUE (A) cursor");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("RMB"), "Set PURPLE (B) cursor");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("h"), "Show this help");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("q"), "Save current demodbuffer");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("x"), "Insert silence between A and B");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("a"), "Mark amplitude between A and B with C and D");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("n"), "Norm the graph values (-128 .. +128)/remove DC offset");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("e"), "Extract demodbuffer (trim to start/end of it)");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("d"), "Insert safe spave after/before demod buffer cut");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("c"), "Copy contents between A and B");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("v"), "Overwrite copied contents after B marker");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("w"), "Remove silence from start / end");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⎋  Togglers: "));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("k"), "Toggle cascade ADC normalization");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("l"), "Toggle lock grid relative to samples");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("f"), "Toggle grid visibility");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("4"), "Toggle slider");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("7"), "Toggle mouseclick between A/B and C/D");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("g"), "Toggle grid and demodulation plot display");
            PrintAndLogEx(NORMAL, "\n" _RED_(" ⍆  Markers: "));
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("m"), "Set C and D markers on A and B");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("b"), "Set A and B markers on C and D");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("r"), "Reset all markers");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("s"), "Swap A and B markers");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("2"), "Insert C marker between A and B");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("3"), "Split samples between A and B into 3 equal pools");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("5"), "Detect modulation");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("6"), "Detect ASK wavelength");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("9"), "Restore initial state of graph");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("0"), "Set A and B to start / end of viewport");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("o"), "Set A and B to start / end of graph");
            PrintAndLogEx(NORMAL, "    %-*s%s", 25 + 9, _MAGENTA_("b"), "Set A and B markers on C and D");
            g_printAndLog = old_printAndLog;
            break;
        }
        case Qt::Key_L:
            g_GridLocked = !g_GridLocked;
            if (g_GridLocked)
                g_GridOffset += (g_GraphStart - unlockStart);
            else
                unlockStart = g_GraphStart;
            break;

        case Qt::Key_T:
            Trim();
            break;

        case Qt::Key_Home:
            g_GraphStart = 0;
            break;

        case Qt::Key_End:
            g_GraphStart = startMax;
            break;

        case Qt::Key_PageUp:
            if (g_GraphStart >= PageWidth) {
                g_GraphStart -= PageWidth;
            } else {
                g_GraphStart = 0;
            }
            break;

        case Qt::Key_PageDown:
            g_GraphStart += PageWidth;
            if (g_GraphStart > startMax)
                g_GraphStart = startMax;
            break;

        default:
            QWidget::keyPressEvent(event);
            return;
            break;
    }

    this->update();
}
