#include <QApplication>
#include <QFileDialog>
#include <QFileInfo>
#include <QMainWindow>
#include <QMenuBar>
#include <QMessageBox>
#include <QPdfDocument>
#include <QPdfView>

class MainWindow : public QMainWindow
{
public:
    MainWindow()
    {
        m_document = new QPdfDocument(this);

        m_view = new QPdfView(this);
        m_view->setDocument(m_document);
        m_view->setPageMode(QPdfView::PageMode::MultiPage);
        m_view->setZoomMode(QPdfView::ZoomMode::FitToWidth);
        setCentralWidget(m_view);

        QMenu *fileMenu = menuBar()->addMenu("&File");
        QAction *openAction = fileMenu->addAction("&Open...");
        openAction->setShortcut(QKeySequence::Open);
        connect(openAction, &QAction::triggered, this, &MainWindow::openFile);

        setWindowTitle("PDF Reader");
        resize(900, 700);
    }

private:
    void openFile()
    {
        QString path = QFileDialog::getOpenFileName(
            this, "Open PDF", QString(), "PDF files (*.pdf)");
        if (path.isEmpty())
            return;

        if (m_document->load(path) != QPdfDocument::Error::None) {
            QMessageBox::warning(this, "Error", "Could not open that PDF.");
            return;
        }
        setWindowTitle(QFileInfo(path).fileName() + " - PDF Reader");
    }

    QPdfDocument *m_document;
    QPdfView *m_view;
};

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    MainWindow window;
    window.show();
    return app.exec();
}