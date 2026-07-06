#include "ui/mainwindow.h"

#include <QApplication>
#include <QCoreApplication>

int main(int argc, char *argv[])
{
    QApplication a(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("MicArray"));
    QCoreApplication::setApplicationName(QStringLiteral("MicArray"));
    MicArray::MainWindow w;
    w.show();
    return QApplication::exec();
}
