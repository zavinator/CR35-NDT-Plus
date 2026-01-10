#include "CR35NDTPlus.h"
#include <QtWidgets/QApplication>

#include "Logger.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    Logger logger("CR35NDTPlus");
    CR35NDTPlus window(logger);
    window.show();
    return app.exec();
}
