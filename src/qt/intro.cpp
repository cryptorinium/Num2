#include "intro.h"
#include "ui_intro.h"
#include "util.h"

#include <QFileDialog>
#include <QSettings>
#include <QMessageBox>

#include <boost/filesystem.hpp>

/* Minimum free space (in bytes) needed for data directory */
static const uint64 GB_BYTES = 1000000000LL;
static const uint64 BLOCK_CHAIN_SIZE = 10LL * GB_BYTES;

/* Check free space asynchronously to prevent hanging the UI thread.

   Up to one request to check a path is in flight to this thread; when the check()
   function runs, the current path is requested from the associated Intro object.
   The reply is sent back through a signal.

   This ensures that no queue of checking requests is built up while the user is
   still entering the path, and that always the most recently entered path is checked as
   soon as the thread becomes available.
*/
class FreespaceChecker : public QObject
{
    Q_OBJECT
public:
    FreespaceChecker(Intro *intro);

    enum Status {
        ST_OK,
        ST_ERROR
    };

public slots:
    void check();

signals:
    void reply(int status, const QString &message, quint64 available);

private:
    Intro *intro;
};

#include "intro.moc"

FreespaceChecker::FreespaceChecker(Intro *intro)
{
    this->intro = intro;
}

void FreespaceChecker::check()
{
    namespace fs = boost::filesystem;
    QString dataDirStr = intro->getPathToCheck();
    fs::path dataDir = fs::path(dataDirStr.toStdString());
    uint64 freeBytesAvailable = 0;
    int replyStatus = ST_OK;
    QString replyMessage = tr("A new data directory will be created.");

    /* Find first parent that exists, so that fs::space does not fail */
    fs::path parentDir = dataDir;
    while(parentDir.has_parent_path() && !fs::exists(parentDir))
    {
        parentDir = parentDir.parent_path();
    }

    try {
        freeBytesAvailable = fs::space(parentDir).available;
        if(fs::exists(dataDir))
        {
            if(fs::is_directory(dataDir))
            {
                QString separator = QDir::toNativeSeparators("/");
                replyStatus = ST_OK;
                replyMessage = tr("Directory already exists. Add <code>%1name</code> if you intend to create a new directory here.").arg(separator);
            } else {
                replyStatus = ST_ERROR;
                replyMessage = tr("Path already exists, and is not a directory.");
            }
        }
    } catch(fs::filesystem_error &e)
    {
        /* Parent directory does not exist or is not accessible */
        replyStatus = ST_ERROR;
        replyMessage = tr("Cannot create data directory here.");
    }
    emit reply(replyStatus, replyMessage, freeBytesAvailable);
}


Intro::Intro(QWidget *parent) :
    QDialog(parent),
    ui(new Ui::Intro),
    thread(0),
    signalled(false)
{
    ui->setupUi(this);
    ui->sizeWarningLabel->setText(ui->sizeWarningLabel->text().arg(BLOCK_CHAIN_SIZE/GB_BYTES));
    startThread();
}

Intro::~Intro()
{
    delete ui;
    /* Ensure thread is finished before it is deleted */
    emit stopThread();
    thread->wait();
}

QString Intro::getDataDirectory()
{
    return ui->dataDirectory->text();
}

void Intro::setDataDirectory(const QString &dataDir)
{
    ui->dataDirectory->setText(dataDir);
    if(dataDir == getDefaultDataDirectory())
    {
        ui->dataDirDefault->setChecked(true);
        ui->dataDirectory->setEnabled(false);
        ui->ellipsisButton->setEnabled(false);
    } else {
        ui->dataDirCustom->setChecked(true);
        ui->dataDirectory->setEnabled(true);
        ui->ellipsisButton->setEnabled(true);
    }
}

QString Intro::getDefaultDataDirectory()
{
    return QString::fromStdString(GetDefaultDataDir().string());
}

void Intro::pickDataDirectory()
{
    namespace fs = boost::filesystem;;
    QSettings settings;
    /* If data directory provided on command line, no need to look at settings
       or show a picking dialog */
    if(!GetArg("-datadir", "").empty())
        return;
    /* 1) Default data directory for operating system */
    QString dataDir = getDefaultDataDirectory();
    /* 2) Allow QSettings to override default dir */
    dataDir = settings.value("strDataDir", dataDir).toString();

    if(!fs::exists(dataDir.toStdString()) || GetBoolArg("-choosedatadir", false))
    {
        /* If current default data directory does not exist, let the user choose one */
        Intro intro;
        intro.setDataDirectory(dataDir);
        while(true)
        {
            if(!intro.exec())
            {
                /* Cancel clicked */
                exit(0);
            }
            dataDir = intro.getDataDirectory();
            try {
                fs::create_directory(dataDir.toStdString());
                break;
            } catch(fs::filesystem_error &e) {
                QMessageBox::critical(0, QObject::tr("Num2coin"),
                                      QObject::tr("Error: Specified data directory \"%1\" can not be created.").arg(dataDir));
                /* fall through, back to choosing screen */
            }
        }

        settings.setValue("strDataDir", dataDir);
    }
    SoftSetArg("-datadir", dataDir.toStdString());
}

void Intro::setStatus(int status, const QString &message, quint64 bytesAvailable)
{
    switch(status)
    {
    case FreespaceChecker::ST_OK:
        ui->errorMessage->setText(message);
        ui->errorMessage->setStyleSheet("");
        break;
    case FreespaceChecker::ST_ERROR:
        ui->errorMessage->setText(tr("Error") + ": " + message);
        ui->errorMessage->setStyleSheet("QLabel { color: #800000 }");
        break;
    }
    /* Indicate number of bytes available */
    if(status == FreespaceChecker::ST_ERROR)
    {
        ui->freeSpace->setText("");
    } else {
        QString freeString = QString::number(bytesAvailable/GB_BYTES) + tr("GB of free space available");
        if(bytesAvailable < BLOCK_CHAIN_SIZE)
        {
            freeString += " " + tr("(of %1GB needed)").arg(BLOCK_CHAIN_SIZE/GB_BYTES);
            ui->freeSpace->setStyleSheet("QLabel { color: #800000 }");
        } else {
            ui->freeSpace->setStyleSheet("");
        }
        ui->freeSpace->setText(freeString+".");
    }
    /* Don't allow confirm in ERROR state */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(status != FreespaceChecker::ST_ERROR);
}

void Intro::on_dataDirectory_textChanged(const QString &dataDirStr)
{
    /* Disable OK button until check result comes in */
    ui->buttonBox->button(QDialogButtonBox::Ok)->setEnabled(false);
    checkPath(dataDirStr);
}

void Intro::on_ellipsisButton_clicked()
{
    QString dir = QFileDialog::getExistingDirectory(0, "Choose data directory", ui->dataDirectory->text());
    if(!dir.isEmpty())
        ui->dataDirectory->setText(dir);
}

void Intro::on_dataDirDefault_clicked()
{
    setDataDirectory(getDefaultDataDirectory());
}

void Intro::on_dataDirCustom_clicked()
{
    ui->dataDirectory->setEnabled(true);
    ui->ellipsisButton->setEnabled(true);
}

void Intro::startThread()
{
    thread = new QThread(this);
    FreespaceChecker *executor = new FreespaceChecker(this);
    executor->moveToThread(thread);

    connect(executor, SIGNAL(reply(int,QString,quint64)), this, SLOT(setStatus(int,QString,quint64)));
    connect(this, SIGNAL(requestCheck()), executor, SLOT(check()));
    /*  make sure executor object is deleted in its own thread */
    connect(this, SIGNAL(stopThread()), executor, SLOT(deleteLater()));
    connect(this, SIGNAL(stopThread()), thread, SLOT(quit()));

    thread->start();
}

void Intro::checkPath(const QString &dataDir)
{
    mutex.lock();
    pathToCheck = dataDir;
    if(!signalled)
    {
        signalled = true;
        emit requestCheck();
    }
    mutex.unlock();
}

QString Intro::getPathToCheck()
{
    QString retval;
    mutex.lock();
    retval = pathToCheck;
    signalled = false; /* new request can be queued now */
    mutex.unlock();
    return retval;
}
