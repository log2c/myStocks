#include <QApplication>

#include "app_controller.h"

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    QApplication::setOrganizationName("myStocks");
    QApplication::setApplicationName("myStocks");

    AppController controller;
    return app.exec();
}
