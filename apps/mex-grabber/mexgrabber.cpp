/* +---------------------------------------------------------------------------+
   |                     Mobile Robot Programming Toolkit (MRPT)               |
   |                          http://www.mrpt.org/                             |
   |                                                                           |
   | Copyright (c) 2005-2014, Individual contributors, see AUTHORS file        |
   | See: http://www.mrpt.org/Authors - All rights reserved.                   |
   | Released under BSD License. See details in http://www.mrpt.org/License    |
   +---------------------------------------------------------------------------+ */


/*-----------------------------------------------------------------------------
    APPLICATION: MEX-grabber
    FILE: mexgrabber.cpp
    AUTHORS: Jesus Briales Garcia <jesusbriales@gmail.com>
             Jose Luis Blanco Claraco <joseluisblancoc@gmail.com>

    For instructions and details, see:
     http://www.mrpt.org/Application:MEX-grabber
  -----------------------------------------------------------------------------*/

#include <mrpt/hwdrivers/CGenericSensor.h>
#include <mrpt/utils/CConfigFile.h>
#include <mrpt/utils/CFileGZOutputStream.h>
#include <mrpt/utils/CImage.h>
#include <mrpt/utils/round.h>
#include <mrpt/slam/CActionCollection.h>
#include <mrpt/slam/CSensoryFrame.h>
#include <mrpt/slam/CObservationOdometry.h>
#include <mrpt/slam/CObservationGPS.h>
#include <mrpt/slam/CObservationIMU.h>
#include <mrpt/slam/CActionRobotMovement2D.h>
#include <mrpt/system/os.h>
#include <mrpt/system/filesystem.h>

// Matlab MEX interface headers
#include <mrpt/mexplus.h>

using namespace mrpt;
using namespace mrpt::system;
using namespace mrpt::hwdrivers;
using namespace mrpt::utils;
using namespace mrpt::slam;
using namespace std;
using namespace mexplus;

const std::string GLOBAL_SECTION_NAME = "global";

// Forward declarations:
struct TThreadParams
{
    CConfigFile		*cfgFile;
    string			sensor_label;
};

void SensorThread(TThreadParams params);

CGenericSensor::TListObservations		global_list_obs;
synch::CCriticalSection					cs_global_list_obs;

bool									allThreadsMustExit = false;

string 		rawlog_ext_imgs_dir;		// Directory where to save externally stored images, only for CCameraSensor's.

// Thread handlers vector stored as global (persistent MEX variables)
vector<TThreadHandle> lstThreads;

// Configuration variables
MRPT_TODO("Set as variable controlled from Matlab")
size_t max_num_obs = 50;

/* Important:
 * All global variables will be stored between MEX calls,
 * and can be used by running threads backstage. */

namespace {
// Defines MEX API for new.
MEX_DEFINE(new) (int nlhs, mxArray* plhs[],
                 int nrhs, const mxArray* prhs[]) {
    printf(" mex-grabber - Part of the MRPT\n");
    printf(" MRPT C++ Library: %s - BUILD DATE %s\n", MRPT_getVersion().c_str(), MRPT_getCompilationDate().c_str());
    printf("-------------------------------------------------------------------\n");

    mexplus::InputArguments input(nrhs, prhs, 1);
//    mexplus::OutputArguments output(nlhs, plhs, 1);

    // Initialize global (persistent) variables
    allThreadsMustExit = false;

    string INI_FILENAME( input.get<string>(0) );
    ASSERT_FILE_EXISTS_(INI_FILENAME)

    CConfigFile iniFile( INI_FILENAME );

    // ------------------------------------------
    //			Load config from file:
    // ------------------------------------------
    string			rawlog_prefix = "dataset";
    int				time_between_launches = 300;
    double			SF_max_time_span = 0.25;			// Seconds
    bool			use_sensoryframes = false;
    int				GRABBER_PERIOD_MS = 1000;
    int 			rawlog_GZ_compress_level  = 1;  // 0: No compress, 1-9: compress level

    MRPT_LOAD_CONFIG_VAR( rawlog_prefix, string, iniFile, GLOBAL_SECTION_NAME );
    MRPT_LOAD_CONFIG_VAR( time_between_launches, int, iniFile, GLOBAL_SECTION_NAME );
    MRPT_LOAD_CONFIG_VAR( SF_max_time_span, float,		iniFile, GLOBAL_SECTION_NAME );
    MRPT_LOAD_CONFIG_VAR( use_sensoryframes, bool,		iniFile, GLOBAL_SECTION_NAME );
    MRPT_LOAD_CONFIG_VAR( GRABBER_PERIOD_MS, int, iniFile, GLOBAL_SECTION_NAME );

    MRPT_LOAD_CONFIG_VAR( rawlog_GZ_compress_level, int, iniFile, GLOBAL_SECTION_NAME );

    // Build full rawlog file name:
    string	rawlog_postfix = "_";

    //rawlog_postfix += dateTimeToString( now() );
    mrpt::system::TTimeParts parts;
    mrpt::system::timestampToParts(now(), parts, true);
    rawlog_postfix += format("%04u-%02u-%02u_%02uh%02um%02us",
                             (unsigned int)parts.year,
                             (unsigned int)parts.month,
                             (unsigned int)parts.day,
                             (unsigned int)parts.hour,
                             (unsigned int)parts.minute,
                             (unsigned int)parts.second );

    rawlog_postfix = mrpt::system::fileNameStripInvalidChars( rawlog_postfix );

    // Only set this if we want externally stored images:
    rawlog_ext_imgs_dir = rawlog_prefix+fileNameStripInvalidChars( rawlog_postfix+string("_Images") );

    // Also, set the path in CImage to enable online visualization in a GUI window:
    CImage::IMAGES_PATH_BASE = rawlog_ext_imgs_dir;


    rawlog_postfix += string(".rawlog");
    rawlog_postfix = fileNameStripInvalidChars( rawlog_postfix );

    string			rawlog_filename = rawlog_prefix + rawlog_postfix;

    cout << endl ;
    cout << "Output rawlog filename: " << rawlog_filename << endl;
    cout << "External image storage: " << rawlog_ext_imgs_dir << endl << endl;

    // ----------------------------------------------
    // Launch threads:
    // ----------------------------------------------
    vector_string	sections;
    iniFile.getAllSections( sections );

    for (vector_string::iterator it=sections.begin();it!=sections.end();++it)
    {
        if (*it==GLOBAL_SECTION_NAME || it->empty() || iniFile.read_bool(*it,"rawlog-grabber-ignore",false,false) )
            continue;	// This is not a sensor:

        //cout << "Launching thread for sensor '" << *it << "'" << endl;

        TThreadParams	threParms;
        threParms.cfgFile		= &iniFile;
        threParms.sensor_label	= *it;

        TThreadHandle	thre = createThread(SensorThread, threParms);

        lstThreads.push_back(thre);
        sleep(time_between_launches);
    }

    printf("Finished threads launch\n");
} // End of "new" method

// Defines MEX API for read (acquire observations in Matlab form)
MEX_DEFINE(read) (int nlhs, mxArray* plhs[],
                  int nrhs, const mxArray* prhs[]) {
    // ----------------------------------------------
    // Run:
    // ----------------------------------------------
//    mexplus::InputArguments input(nrhs, prhs, 1);
    mexplus::OutputArguments output(nlhs, plhs, 1);

    CGenericSensor::TListObservations	copy_of_global_list_obs;

    // See if we have observations and process them:
    {
        synch::CCriticalSectionLocker	lock (&cs_global_list_obs);
        copy_of_global_list_obs.clear();

        if (!global_list_obs.empty())
        {
            CGenericSensor::TListObservations::iterator itEnd = global_list_obs.begin();
            std::advance( itEnd, global_list_obs.size() / 2 );
            copy_of_global_list_obs.insert(global_list_obs.begin(),itEnd );
            global_list_obs.erase(global_list_obs.begin(), itEnd);
        }
    }	// End of cs lock

    // Read from list of observations to mxArray cell array (store any kind of objects)
    MxArray cell_obs( MxArray::Cell(1, copy_of_global_list_obs.size()) );
    size_t index = 0;
    for (CGenericSensor::TListObservations::iterator it=copy_of_global_list_obs.begin(); it!=copy_of_global_list_obs.end();++it)
    {
        MRPT_TODO("Bug when applied in second round! No image is stored!?")
        MxArray struct_obs( it->second->writeToMatlab() );
        struct_obs.set("ts", it->first); // Store timestamp too
        cell_obs.set( index, struct_obs.release() );
        //cell_obs.set( index, it->second->writeToMatlab() );
        index++;
    }

    // Returns created struct
    output.set(0, cell_obs.release());

    // No need to sleep, since this function only applies when user requested from Matlab
} // End of "read" method

// Defines MEX API for delete.
MEX_DEFINE(delete) (int nlhs, mxArray* plhs[],
                    int nrhs, const mxArray* prhs[]) {
    //    mexplus::InputArguments input(nrhs, prhs, 1);
    //    mexplus::OutputArguments output(nlhs, plhs, 0);
    if (allThreadsMustExit)
    {
        printf("[main thread] Ended due to other thread signal to exit application.\n");
    }

    // Wait all threads:
    // ----------------------------
    allThreadsMustExit = true;
    mrpt::system::sleep(300);
    printf("\nWaiting for all threads to close...\n");
    for (vector<TThreadHandle>::iterator th=lstThreads.begin();th!=lstThreads.end();++th)
        joinThread( *th );

    cout << endl << "rawlog-grabber application finished" << endl;
} // End of "delete" method

} // End of namespace

MEX_DISPATCH // Don't forget to add this if MEX_DEFINE() is used.

// ------------------------------------------------------
//				SensorThread
// ------------------------------------------------------
void SensorThread(TThreadParams params)
{
    try
    {
        string driver_name = params.cfgFile->read_string(params.sensor_label,"driver","",true);

        CGenericSensorPtr	sensor = CGenericSensor::createSensorPtr(driver_name );
        if (!sensor)
        {
            cerr << endl << "***ERROR***: Class name not recognized: " << driver_name << endl;
            allThreadsMustExit = true;
        }

        // Load common & sensor specific parameters:
        sensor->loadConfig( *params.cfgFile, params.sensor_label );

        cout << format("[thread_%s] Starting...",params.sensor_label.c_str()) << " at " << sensor->getProcessRate() <<  " Hz" << endl;

        ASSERTMSG_(sensor->getProcessRate()>0,"process_rate must be set to a valid value (>0 Hz).");
        int		process_period_ms = round( 1000.0 / sensor->getProcessRate() );

        // For imaging sensors, set external storage directory:
        sensor->setPathForExternalImages( rawlog_ext_imgs_dir );

        // Init device:
        sensor->initialize();


        while (! allThreadsMustExit )
        {
            TTimeStamp t0= now();

            // Process
            sensor->doProcess();

            // Get new observations
            CGenericSensor::TListObservations	lstObjs;
            sensor->getObservations( lstObjs );

            {
                synch::CCriticalSectionLocker	lock (&cs_global_list_obs);
                // Control maximum number of stored observations to prevent excesive growth of list between calls
                if ( global_list_obs.size() < 2 * max_num_obs ) // .size() is returning 2 countings for each pair
                    global_list_obs.insert( lstObjs.begin(), lstObjs.end() );
            }

            lstObjs.clear();

            // wait until the process period:
            TTimeStamp t1= now();
            double	At = timeDifference(t0,t1);
            int At_rem_ms = process_period_ms - At*1000;
            if (At_rem_ms>0)
                sleep(At_rem_ms);
        }

        sensor.clear();
        cout << format("[thread_%s] Closing...",params.sensor_label.c_str()) << endl;
    }
    catch (std::exception &e)
    {
        cerr << e.what() << endl;
        allThreadsMustExit = true;
    }
    catch (...)
    {
        cerr << "Untyped exception!!" << endl;
        allThreadsMustExit = true;
    }
}

// ------------------------------------------------------
//					MAIN THREAD
//
// For testing outside Matlab
// ------------------------------------------------------
int main(int argc, const char* argv[] )
{
    try
    {
        printf(" MEX-grabber - Part of the MRPT\n");
        printf(" MRPT C++ Library: %s - BUILD DATE %s\n", MRPT_getVersion().c_str(), MRPT_getCompilationDate().c_str());
        printf("-------------------------------------------------------------------\n");
        printf(" This is a test for Matlab MEX functionalities\n");
        printf("-------------------------------------------------------------------\n");

        // Launch threads with "new"
        const mxArray* mxIn[2];
        mxIn[0] = mexplus::MxArray::from( "new" );
        mxIn[1] = mexplus::MxArray::from( argv[1] ); // Read config file path, first argv is function name
        mxArray* mxOut[1];
        mexFunction( 1, mxOut, 2, mxIn );

        // Read frames with "read"
        mxIn[0] = mexplus::MxArray::from( "read" );
        mexFunction( 1, mxOut, 1, mxIn );

        // Finish applicatin with "delete"
        mxIn[0] = mexplus::MxArray::from( "delete" );
        mexFunction( 1, mxOut, 1, mxIn );

        // Repete whole process
        // Launch threads with "new"
        mxIn[0] = mexplus::MxArray::from( "new" );
        mxIn[1] = mexplus::MxArray::from( argv[1] ); // Read config file path, first argv is function name
        mexFunction( 1, mxOut, 2, mxIn );

        // Read frames with "read"
        mxIn[0] = mexplus::MxArray::from( "read" );
        mexFunction( 1, mxOut, 1, mxIn );

        // Finish applicatin with "delete"
        mxIn[0] = mexplus::MxArray::from( "delete" );
        mexFunction( 1, mxOut, 1, mxIn );

        return 0;
    } catch (std::exception &e)
    {
        std::cerr << e.what() << std::endl << "Program finished for an exception!!" << std::endl;
        mrpt::system::pause();
        return -1;
    }
    catch (...)
    {
        std::cerr << "Untyped exception!!" << std::endl;
        mrpt::system::pause();
        return -1;
    }
}
