#pragma once

#include <QtWidgets/QMainWindow>
#include "ui_CR35NDTPlus.h"

#include "CR35Device.h" 

class CR35NDTPlus : public QMainWindow {
    Q_OBJECT

public:
    CR35NDTPlus(Logger& logger, QWidget* parent = nullptr);

private slots:

    void saveImage(const QImage&);

private:
    Ui::CR35NDTPlusClass ui;

    CR35Device m_device;
};

