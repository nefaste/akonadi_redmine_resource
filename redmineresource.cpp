/*
 * © 2011 Romain Lalaut <romain.lalaut@laposte.net>
 * © 2014 Mario Bensi <mbensi@ipsquad.net>
 * License: GPL-2+
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 */

#include "redmineresource.h"

#include "settings.h"
#include "settingsadaptor.h" 
#include <kconfigdialog.h>

#include <QtDBus/QDBusConnection>
#include <QDebug>

using namespace Akonadi;

#define LIMIT 100 // limit of issues per page

redmineResource::redmineResource( const QString &id )
  : ResourceBase( id )
{
  new SettingsAdaptor( Settings::self() );
  QDBusConnection::sessionBus().registerObject( QLatin1String( "/Settings" ),
                            Settings::self(), QDBusConnection::ExportAdaptors );
  
  connect (this, SIGNAL( userChanged() ), this, SLOT(retrieveUser()));

  mimeTypes << QLatin1String("text/calendar");
  mimeTypes << QLatin1String("application/x-vnd.akonadi.calendar.todo");
  if(!Settings::self()->endpoint().isEmpty()){
    emit userChanged();
  }
}

redmineResource::~redmineResource()
{
}

KUrl redmineResource::url( const QString &path )
{
  QString url = Settings::self()->endpoint() + path;
  kWarning() << url;
  return KUrl(url);
}

void redmineResource::retrieveUser()
{
  kWarning() << "retrieve user "; 
  
  userBuffer.clear();
  
  KIO::TransferJob *job = KIO::get(url("/users/current.xml?include=memberships"));
  connect (job, SIGNAL( data(KIO::Job *, const QByteArray & ) ), this, SLOT(userDataReceived(KIO::Job *,const QByteArray &)));
  connect (job, SIGNAL( result(KJob*) ) , this, SLOT( userDataResult(KJob *) ));

}

void redmineResource::userDataReceived(KIO::Job* job, const QByteArray&  data)
{
    Q_UNUSED(job);
//   kWarning() << "user data received : " << data.length() << " bytes";
  
  if(data.isEmpty()){
//     kWarning() << "empty data : skip";
    return;
  }
  
  userBuffer.append(data);
}

void redmineResource::userDataResult(KJob* job)
{
  kWarning() << "user data result";
  
  if(job->error()){
    kWarning() << job->errorString();
    return;
  }
  
  if(userBuffer.isEmpty()){
    kWarning() << "empty buffer : skip";
    return;
  }

  QDomDocument doc;
  doc.setContent(userBuffer);
  QDomElement docEl = doc.documentElement(); 
  
  userId = readEl(docEl, "id");
  kWarning() << "user ID : " << userId;
 
  synchronize(); 
}

void redmineResource::retrieveCollections()
{
  kWarning() << "retrieve collections";
  
  collectionsBuffer = "";
  
  KIO::TransferJob *job = KIO::get(url("/projects.xml"));
  connect (job, SIGNAL(  data(KIO::Job *, const QByteArray & ) ), this, SLOT(collectionsDataReceived(KIO::Job *,const QByteArray &)));
  connect (job, SIGNAL( result( KJob * ) ), this, SLOT( collectionsDataResult(KJob *) ));
}

void redmineResource::collectionsDataReceived(KIO::Job* job, const QByteArray& data )
{
    Q_UNUSED(job);
//   kWarning() << "collections data received : " << QString::number(data.length());
  
  if(data.isEmpty()){
//     kWarning() << "empty data : skip";
    return;
  }
  
  collectionsBuffer.append(data);
}

void redmineResource::collectionsDataResult(KJob* job)
{
  kWarning() << "collections received";
  
  if ( job->error() ){
      kWarning() << job->errorString();
      return;
  }
  
  if ( collectionsBuffer.isEmpty() ){
    kWarning() << "empty buffer : skip";
    return;
  }
  
  QDomDocument doc;
  doc.setContent(collectionsBuffer);
  QDomElement docEl = doc.documentElement(); 
  QDomNodeList nodes  = docEl.elementsByTagName("project"); 
  Collection::List collectionList;
  QStringList contentTypes;
  contentTypes.append(mimeTypes);
  contentTypes.append(Akonadi::Collection::mimeType());
  
  uint j = nodes.length();
  kWarning() << "projects found :" << j;
  for(uint i=0; i<j; ++i){
    
    QDomElement el = nodes.item(i).toElement();
    
    Collection collection;
    collection.setRemoteId( readEl(el, "id") );
    collection.setParentCollection( Collection::root() );
    collection.setName( i18n("Project") + " - " + readEl(el, "name") );
    collection.setContentMimeTypes(contentTypes);
    kWarning() << "collection :" << collection.name();
    collectionList << collection;
  }
  
  collectionsRetrieved( collectionList );
}

void redmineResource::retrieveItems( const Akonadi::Collection &collection )
{
  kWarning() << "retrieve items for collection " << collection.name(); 
  
  parent_id.clear();
  childList.clear();

  KJob *job = createIssuesJob(collection.remoteId(), QString(userId));
  
  itemsBuffers[job] = "";
  kWarning() << "new job " << job;

  KCalCore::Todo::Ptr todo(new KCalCore::Todo);
  todo->setSummary(collection.name());
  todo->setCustomProperty("Zanshin", "Project", "1");
  qDebug() << "create root to for projetid" << collection.remoteId() << "name " << collection.name();
  projectMap[collection.remoteId()] = todo;
  Item item("application/x-vnd.akonadi.calendar.todo");
  item.setRemoteId(collection.remoteId());
  item.setPayload<KCalCore::Todo::Ptr>(todo);

  globalItems << item;
  
  connect (job, SIGNAL(  data(KIO::Job *, const QByteArray & )), this, SLOT(itemsDataReceived(KIO::Job *,const QByteArray &)));
  connect (job, SIGNAL( result( KJob * ) ), this, SLOT( itemsDataResult(KJob *) ));
}

void redmineResource::itemsDataReceived(KIO::Job* job, const QByteArray& data )
{
//   kWarning() << "items data received. job : " << job;
  
  if( data.isEmpty() ){
//     kWarning() << "empty data : skip";
    return;
  }
  
  itemsBuffers[job].append(data);
//   kWarning() << "writting " << data.length() << " bytes into the buffer (total : " << itemsBuffers[job].length() << " bytes)";
}

void redmineResource::itemsDataResult(KJob* job)
{ 
  kWarning() << "items result. job : " << job;
  
  Item::List items;
  int total = 0;
  int limit = 0;
  int offset = 0;
  QString projectID;
  bool fetchMoreIssues = true;
  
  if ( job->error() ){
      kError() << job->errorString();
  } else {
  
    kWarning() << itemsBuffers[job].length() << "bytes";
      
    QByteArray data = itemsBuffers[job];
    
    if( data.isEmpty() ){
      kWarning() << "empty buffer : skip";
    } else {
  
      projectID = static_cast<KIO::TransferJob*>(job)->url().queryItem("project_id");
      KCalCore::Todo::Ptr projectTodo = projectMap[projectID];

      QDomDocument doc;
      doc.setContent(data);

      QDomElement issues  = doc.firstChildElement("issues");
      total = issues.attribute("total_count").toInt();
      limit = issues.attribute("limit").toInt();
      offset = issues.attribute("offset").toInt();

      QDomElement docEl = doc.documentElement(); 
      QDomNodeList nodes  = docEl.elementsByTagName("issue"); 
      
      uint j = nodes.length();
      kWarning() << "issues found :" << j;
      
      if(j==0){
        kWarning() << data;
      }
      
      for(uint i=0; i<j; ++i){
        
        QDomElement el = nodes.item(i).toElement();
        
        KCalCore::Todo::Ptr todo(new KCalCore::Todo);
        todo->setSummary(readEl(el, "subject"));
        todo->setDescription(readEl(el, "description"));
        if(hasEl(el, "due_date")){
          todo->setDtDue(KDateTime(readElDate(el, "due_date")));
        }
        if(hasEl(el, "start_date")){
          todo->setDtStart(KDateTime(readElDate(el, "start_date")));
        }
        if(hasEl(el, "done_ratio")){
          todo->setPercentComplete((int) readEl(el, "done_ratio").toFloat()*100);
        }
        
        QDomNodeList nodes = el.elementsByTagName("parent");
        if(!nodes.isEmpty()){
          QDomNamedNodeMap map = nodes.item(0).attributes();
          QDomAttr attr = map.namedItem("id").toAttr();
          QString id = attr.value();
          if (parent_id.contains(id)) {
            todo->setRelatedTo(parent_id[id]);
          } else {
            childList.insert(id, todo);
            // set temporaly a root parent
            todo->setRelatedTo(projectTodo->uid());
          }
        } else {
          todo->setRelatedTo(projectTodo->uid());
        }

        QString id = readEl(el, "id");
        Item item("application/x-vnd.akonadi.calendar.todo");
        item.setRemoteId(id);
        item.setPayload<KCalCore::Todo::Ptr>(todo);

        parent_id[id] = todo->uid();
        
        if (childList.contains(id)) {
          QList<KCalCore::Todo::Ptr> children = childList.values(id);
          for (int i = 0; i < children.size(); ++i) {
              children.at(i)->setRelatedTo(todo->uid());
          }
        }

        items << item;
        if (Settings::self()->limit() > 0
         && total > Settings::self()->limit()
         && globalItems.size() + items.size() == (Settings::self()->limit() + 1)) {
            fetchMoreIssues = false;
            break;
        }
      }
    }
  }
  itemsBuffers.remove(job);

  globalItems << items;

  qDebug() << "projectID" << projectID << "total" << total << "globalItems" << globalItems.size() << "items" << items.size() << "fetchMoreIssues" << fetchMoreIssues;

  if (fetchMoreIssues && ((total - (offset + limit)) > 0) && (items.size() == limit)) {
    KJob* nextJob = createIssuesJob(projectID, QString(userId), offset + limit);
    itemsBuffers[nextJob] = "";

    connect (nextJob, SIGNAL(  data(KIO::Job *, const QByteArray & )), this, SLOT(itemsDataReceived(KIO::Job *,const QByteArray &)));
    connect (nextJob, SIGNAL( result( KJob * ) ), this, SLOT( itemsDataResult(KJob *) ));
  } else {
    itemsRetrieved( globalItems );
    globalItems.clear();
  }
}

bool redmineResource::retrieveItem( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
  kWarning() << "retrieve item : " << item.url() << ". Collection : " << parts; 

  // TODO: this method is called when Akonadi wants more data for a given item.
  // You can only provide the parts that have been requested but you are allowed
  // to provide all in one go

  return true;
}

void redmineResource::aboutToQuit()
{
  // TODO: any cleanup you need to do while there is still an active
  // event loop. The resource will terminate after this method returns
}

void redmineResource::configure( WId windowId )
{
  Q_UNUSED( windowId );

  if ( KConfigDialog::showDialog( "settings" ) )
    return; 
  
  KConfigDialog* dialog = new KConfigDialog(0,"settings",
                                          Settings::self()); 
  QWidget *widget = new QWidget;
  Ui::SettingsForm ui;
  ui.setupUi(widget); 
 
  dialog->addPage( widget, i18n("Connection"), "connection" ); 
  
  dialog->show();
  
  connect (dialog, SIGNAL(settingsChanged(QString)), this, SIGNAL(userChanged()));
 
  synchronize(); 
}

void redmineResource::itemAdded( const Akonadi::Item &item, const Akonadi::Collection &collection )
{
  kWarning() << "item added : " << item.url() << ". Collection : " << collection.name(); 

  // TODO: this method is called when somebody else, e.g. a client application,
  // has created an item in a collection managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
  changeCommitted( item );
}

void redmineResource::itemChanged( const Akonadi::Item &item, const QSet<QByteArray> &parts )
{
  kWarning() << "item changed : " << item.url() << ". Parts : " << parts; 

  // TODO: this method is called when somebody else, e.g. a client application,
  // has changed an item managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
  changeCommitted( item );
}

void redmineResource::itemRemoved( const Akonadi::Item &item )
{
  kWarning() << "item removed : " << item.url(); 

  // TODO: this method is called when somebody else, e.g. a client application,
  // has deleted an item managed by your resource.

  // NOTE: There is an equivalent method for collections, but it isn't part
  // of this template code to keep it simple
  changeCommitted( item );
}

bool redmineResource::hasEl(const QDomElement& el, const QString &name )
{
  return readEl(el, name) != "";
}

QString redmineResource::readEl(const QDomElement& el, const QString &name )
{
  QDomNodeList nodes = el.elementsByTagName(name);
  if(!nodes.isEmpty()){
    QString value = nodes.item(0).firstChild().toText().data();
    return value;
  } else {
    kWarning() << "cannot read element : " << name;
    return "";
  }
}

QDate redmineResource::readElDate(const QDomElement& el, const QString &name )
{
  QStringList str = readEl(el, name).split('-');
  return QDate(str[0].toUInt(), str[1].toUInt(), str[2].toUInt());
}

KJob* redmineResource::createIssuesJob(QString projectId, QString userId, int offset)
{
     KIO::TransferJob *job = KIO::get(url("/issues.xml?project_id="+projectId
                  +"&subproject_id=!*"
                  +"&assigned_to="+userId
                  +"&offset="+QString::number(offset)
                  +"&limit="+QString::number(LIMIT)), KIO::Reload);
     return job;
}

AKONADI_RESOURCE_MAIN( redmineResource )

#include "redmineresource.moc"
