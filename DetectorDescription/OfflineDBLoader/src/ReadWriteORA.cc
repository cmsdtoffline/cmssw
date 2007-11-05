#include "DetectorDescription/OfflineDBLoader/interface/ReadWriteORA.h"

#include "DataSvc/RefException.h"

#include "CondCore/DBCommon/interface/DBSession.h"
#include "CondCore/DBCommon/interface/Exception.h"
#include "CondCore/DBCommon/src/ServiceLoader.h"
#include "CondCore/DBCommon/interface/ConnectMode.h"
#include "CondCore/DBCommon/interface/MessageLevel.h"
#include "CondCore/MetaDataService/interface/MetaData.h"
//#include "CondCore/DBCommon/interface/DBWriter.h"
#include "CondCore/IOVService/src/IOV.h"
#include "CondCore/IOVService/interface/IOVService.h"
#include "CondCore/IOVService/interface/IOVEditor.h"
#include "CondCore/DBCommon/interface/Time.h"
#include "CondCore/DBCommon/interface/RelationalStorageManager.h"
#include "CondCore/DBCommon/interface/PoolStorageManager.h"
#include "CondCore/DBCommon/interface/Ref.h"

#include "FWCore/Framework/interface/IOVSyncValue.h"
#include "FWCore/MessageLogger/interface/MessageLogger.h"

#include "DetectorDescription/Parser/interface/DDLParser.h"
#include "DetectorDescription/Parser/interface/FIPConfiguration.h"
#include "CondFormats/IdealGeometryObjects/interface/PersistentDDDObjects.h"
#include "DetectorDescription/DBReader/interface/DDORAReader.h"
#include "DetectorDescription/PersistentDDDObjects/interface/DDDToPersFactory.h"
#include "DetectorDescription/Core/interface/DDCompactView.h"
#include "DetectorDescription/Core/interface/DDRoot.h"
#include "DetectorDescription/Core/interface/DDSolid.h"
#include "DetectorDescription/Core/interface/DDSpecifics.h"
#include "DetectorDescription/Core/interface/DDsvalues.h"
#include "DetectorDescription/Core/interface/DDPartSelection.h"
#include "DetectorDescription/Core/interface/DDTransform.h"

#include "POOLCore/POOLContext.h"
#include "CoralBase/Exception.h"

#include<string>

using cond::MetaData;

ReadWriteORA::ReadWriteORA ( const std::string& dbConnectString
			     , const std::string& metaName 
			     , const std::string& userName
			     , const std::string& password
			     , int rotNumSeed )
  : dbConnectString_(dbConnectString)
    , metaName_(metaName)
    , userName_(userName)
    , password_(password)
    , rotNumSeed_(rotNumSeed)
{ 
  //FIXME: check DB and xml for existence, nothing more.
}

ReadWriteORA::~ReadWriteORA () { }

/// Write DB from compactview
bool ReadWriteORA::writeDB ( const DDCompactView & cpv ) {

  std::string token;
  PIdealGeometry* pgeom = new PIdealGeometry;

  try {
    
    DDCompactView::graph_type gra = cpv.graph();
    
    DDMaterial::iterator<DDMaterial> it(DDMaterial::begin()), ed(DDMaterial::end());

    DDDToPersFactory dddFact;

    PMaterial* pm;
    for (; it != ed; ++it) {
      if (! it->isDefined().second) continue;
      pm = dddFact.material ( *it );
      pgeom->pMaterials.push_back ( *pm );
      delete pm;
    }

    DDRotation::iterator<DDRotation> rit(DDRotation::begin()), red(DDRotation::end());
    PRotation* pr;
    DDRotation rotn(DDName("IDENTITYDB","generatedForDB"));
    if ( !rotn.isDefined().second ) {
      DDRotationMatrix* rotID = new DDRotationMatrix();
      DDRotation mydr = DDrot (DDName("IDENTITYDB","generatedForDB"), rotID);
      pr = dddFact.rotation ( mydr );
      pgeom->pRotations.push_back ( *pr );
    }
    for (; rit != red; ++rit) {
      if (! rit->isDefined().second) continue;
      // if it is the identity...
      if ( *(rit->matrix()) == *(rotn.matrix()) ) continue;
      pr = dddFact.rotation( *rit );
      pgeom->pRotations.push_back ( *pr );
    } 


    DDSolid::iterator<DDSolid> sit(DDSolid::begin()), sed(DDSolid::end());
    PSolid* ps;
    for (; sit != sed; ++sit) {
      if (! sit->isDefined().second) continue;  
      ps = dddFact.solid( *sit );
      pgeom->pSolids.push_back( *ps );
      delete ps;
    }

    // Discovered during validation:  If a user declares
    // a LogicalPart in the DDD XML and does not position it
    // in the graph, it will NOT be stored to the database
    // subsequently SpecPars (see below) that use wildcards
    // that may have selected the orphaned LogicalPart node
    // will be read into the DDD from the DB (not by this
    // code, but in the system) and so DDSpecifics will
    // throw a DDException.
    typedef graph_type::const_adj_iterator adjl_iterator;
    adjl_iterator git = gra.begin();
    adjl_iterator gend = gra.end();    
    
    graph_type::index_type i=0;
    PLogicalPart* plp;
    for (; git != gend; ++git) 
      {
	const DDLogicalPart & ddLP = gra.nodeData(git);
	//	std::cout << ddLP << std::endl;
	plp = dddFact.logicalPart ( ddLP  );
	pgeom->pLogicalParts.push_back( *plp );
	delete plp;
	++i;
	if (git->size()) 
	  {
	    // ask for children of ddLP  
	    graph_type::edge_list::const_iterator cit  = git->begin();
	    graph_type::edge_list::const_iterator cend = git->end();
	    PPosPart* ppp;
	    for (; cit != cend; ++cit) 
	      {
		const DDLogicalPart & ddcurLP = gra.nodeData(cit->first);
		ppp = dddFact.position ( ddLP, ddcurLP, gra.edgeData(cit->second), *pgeom, rotNumSeed_ );
// 		std::cout << "okay after the factory..." << std::endl;
		pgeom->pPosParts.push_back( *ppp );
// 		std::cout << "okay after the push_back" << std::endl;
		delete ppp;
// 		std::cout << "okay after the delete..." << std::endl;
	      } // iterate over children
	  } // if (children)
      } // iterate over graph nodes  
  
    std::vector<std::string> partSelections;
    std::map<std::string, std::vector<std::pair<std::string, double> > > values;
    std::map<std::string, int> isEvaluated;


  
    PSpecPar* psp;

    DDSpecifics::iterator<DDSpecifics> spit(DDSpecifics::begin()), spend(DDSpecifics::end());

    // ======= For each DDSpecific...
    for (; spit != spend; ++spit) {
      if ( !spit->isDefined().second ) continue;  
      psp = dddFact.specpar( *spit );
      pgeom->pSpecPars.push_back( *psp );
      delete psp;
    } 

    pgeom->pStrings = dddFact.pstrs.pStrings;
    pgeom->pStartNode = DDRootDef::instance().root().toString();

    pool::POOLContext::loadComponent( "SEAL/Services/MessageService" );
    pool::POOLContext::setMessageVerbosityLevel( seal::Msg::Error );

    cond::ServiceLoader* loader=new cond::ServiceLoader;
    std::string usr = "CORAL_AUTH_USER="+userName_;
    std::string pass = "CORAL_AUTH_PASSWORD="+password_;
    ::putenv(const_cast<char*>(usr.c_str()));
    ::putenv(const_cast<char*>(pass.c_str()));

    loader->loadAuthenticationService( cond::Env );
    loader->loadMessageService( cond::Error );
    cond::DBSession* session=new cond::DBSession();
    //    session->setCatalog("file:PoolFileCatalog.xml");
    //    session->connect(cond::ReadWriteCreate);
    session->open();

    cond::PoolStorageManager psm(dbConnectString_, "file:PoolFileCatalog.xml", session);

    //    session->startUpdateTransaction();
    //    std::string tok=pw.markWrite<PIdealGeometry>(pgeom);
    psm.connect();
    psm.startTransaction(false);
    cond::Ref<PIdealGeometry> geom(psm, pgeom);
    geom.markWrite("PIdealGeometry");
    std::string tok=geom.token();

    cond::Time_t myTime=(unsigned long long)edm::IOVSyncValue::endOfTime().eventID().run();
    cond::IOVService itsIOV(psm);//, myTime);
    cond::IOVEditor* myeditor = itsIOV.newIOVEditor();
    myeditor->insert( myTime, tok );
    tok = myeditor->token(); // replace the payload token with the IOV token for the metadata later.
    psm.commit();


//     cond::IOVIterator* iovit = itsIOV.newIOVIterator( aToken );
//     std::string plT = iovit->payloadToken();

    //    std::string tok = geom.token();

//     //    std::cout << "The end-of-time is " << myTime << std::endl;
//     initiov->iov.insert(std::make_pair(myTime,tok));
//     std::string iovtok = iovw.markWrite<cond::IOV>(initiov);
//     session->commit();
//    session->disconnect();
//    delete session;


    psm.disconnect();

    cond::RelationalStorageManager rsm(dbConnectString_, session);
    //    coral::ISessionProxy* ts = rsm.connect( cond::ReadWriteCreate );
    rsm.connect( cond::ReadWriteCreate );
    rsm.startTransaction( false ); // true == readOnly
//     cond::DBWriter pw(*session, "PIdealGeometry");
//     cond::DBWriter iovw(*session, "IOV");
//    cond::IOV* initiov=new cond::IOV;
    cond::MetaData metadata_svc(rsm); //dbConnectString_, *loader);
    //    metadata_svc.connect(cond::ReadWriteCreate);
    metadata_svc.addMapping(metaName_, tok);
    rsm.commit();
    rsm.disconnect();
    //    metadata_svc.disconnect();
    edm::LogInfo ("DDDReadWriteORA") << "Done with save, token " << tok << " as metaName " << metaName_  << std::endl;

//   if( timetype=="timestamp" ){
//     m_iovservice=new cond::IOVService(*m_pooldb,cond::timestamp);
//   }else{
//     m_iovservice=new cond::IOVService(*m_pooldb,cond::runnumber);
//   }


    delete session;
    delete loader;

  } catch (DDException& e) {
    std::cout << "ReadWriteORA::writeDB caught DDException: " << e.what() << std::endl;
    return false;
  } catch (const coral::Exception& e) {
    std::cout << "ReadWriteORA::writeDB caught coral::Exception: " << e.what() << std::endl;
    return false;
  } catch( const pool::RefException& er){
    std::cerr<<"ReadWriteORA::writeDB caught pool::RefException "<<er.what()<<std::endl;
    return false;
  } catch ( pool::Exception& e ) {
    std::cout << "ReadWriteORA::writeDB caught pool::Exception -> " << e.what() << std::endl;
    return false;
  } catch ( std::exception& e ) {
    std::cout << "ReadWriteORA::writeDB caught std::exception -> " << e.what() << std::endl;
    return false;
  } catch ( ... ) {
    std::cout << "ReadWriteORA::writeDB caught ( ... ) , i.e. unknown, exception. " << std::endl;
    return false;
  }

  return true;
}
