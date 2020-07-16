// This file is part of the AliceVision project.
// Copyright (c) 2017 AliceVision contributors.
// This Source Code Form is subject to the terms of the Mozilla Public License,
// v. 2.0. If a copy of the MPL was not distributed with this file,
// You can obtain one at https://mozilla.org/MPL/2.0/.

#include <aliceVision/sfmData/SfMData.hpp>
#include <aliceVision/sfmData/colorize.hpp>
#include <aliceVision/sfmDataIO/sfmDataIO.hpp>
#include <aliceVision/fuseCut/LargeScale.hpp>
#include <aliceVision/fuseCut/ReconstructionPlan.hpp>
#include <aliceVision/fuseCut/DelaunayGraphCut.hpp>
#include <aliceVision/mesh/meshPostProcessing.hpp>
#include <aliceVision/mvsData/Point3d.hpp>
#include <aliceVision/mvsData/Rgb.hpp>
#include <aliceVision/mvsData/StaticVector.hpp>
#include <aliceVision/mvsUtils/common.hpp>
#include <aliceVision/mvsUtils/MultiViewParams.hpp>
#include <aliceVision/mvsUtils/fileIO.hpp>
#include <aliceVision/system/cmdline.hpp>
#include <aliceVision/system/Logger.hpp>
#include <aliceVision/system/main.hpp>
#include <aliceVision/system/Timer.hpp>

#include <boost/program_options.hpp>
#include <boost/filesystem.hpp>

// These constants define the current software version.
// They must be updated when the command line is changed.
#define ALICEVISION_SOFTWARE_VERSION_MAJOR 4
#define ALICEVISION_SOFTWARE_VERSION_MINOR 0

using namespace aliceVision;

namespace fs = boost::filesystem;
namespace po = boost::program_options;

enum EPartitioningMode
{
    ePartitioningUndefined = 0,
    ePartitioningSingleBlock = 1,
    ePartitioningAuto = 2,
};

EPartitioningMode EPartitioning_stringToEnum(const std::string& s)
{
    if(s == "singleBlock")
        return ePartitioningSingleBlock;
    if(s == "auto")
        return ePartitioningAuto;
    return ePartitioningUndefined;
}

inline std::istream& operator>>(std::istream& in, EPartitioningMode& out_mode)
{
    std::string s;
    in >> s;
    out_mode = EPartitioning_stringToEnum(s);
    return in;
}

enum ERepartitionMode
{
    eRepartitionUndefined = 0,
    eRepartitionMultiResolution = 1,
};

ERepartitionMode ERepartitionMode_stringToEnum(const std::string& s)
{
    if(s == "multiResolution")
        return eRepartitionMultiResolution;
    return eRepartitionUndefined;
}

inline std::istream& operator>>(std::istream& in, ERepartitionMode& out_mode)
{
    std::string s;
    in >> s;
    out_mode = ERepartitionMode_stringToEnum(s);
    return in;
}

/// Create a dense SfMData based on reference \p sfmData, 
/// using \p vertices as landmarks and \p ptCams as observations
void createDenseSfMData(const sfmData::SfMData& sfmData,
                        const mvsUtils::MultiViewParams& mp,
                        const std::vector<Point3d>& vertices,
                        const StaticVector<StaticVector<int>>& ptsCams,
                        sfmData::SfMData& outSfmData
)
{
  outSfmData = sfmData;
  outSfmData.getLandmarks().clear();

  const double unknownScale = 0.0;
  for(std::size_t i = 0; i < vertices.size(); ++i)
  {
    const Point3d& point = vertices.at(i);
    const Vec3 pt3D(point.x, point.y, point.z);
    sfmData::Landmark landmark(pt3D, feature::EImageDescriberType::UNKNOWN);
    // set landmark observations from ptsCams if any
    if(!ptsCams[i].empty())
    {
      for(int cam : ptsCams[i])
      {
        const sfmData::View& view = sfmData.getView(mp.getViewId(cam));
        const camera::IntrinsicBase* intrinsicPtr = sfmData.getIntrinsicPtr(view.getIntrinsicId());
        const sfmData::Observation observation(intrinsicPtr->project(sfmData.getPose(view).getTransform(), pt3D, true), UndefinedIndexT, unknownScale); // apply distortion
        landmark.observations[view.getViewId()] = observation;
      }
    }
    outSfmData.getLandmarks()[i] = landmark;
  }
}

/// Remove all landmarks without observations from \p sfmData.
void removeLandmarksWithoutObservations(sfmData::SfMData& sfmData)
{
  auto& landmarks = sfmData.getLandmarks();
  for(auto it = landmarks.begin(); it != landmarks.end();)
  {
    if(it->second.observations.empty())
      it = landmarks.erase(it);
    else
      ++it;
  }
}


int aliceVision_main(int argc, char* argv[])
{
    system::Timer timer;

    std::string verboseLevel = system::EVerboseLevel_enumToString(system::Logger::getDefaultVerboseLevel());
    std::string sfmDataFilename;
    std::string outputMesh;
    std::string outputDensePointCloud;
    std::string depthMapsFolder;
    EPartitioningMode partitioningMode = ePartitioningSingleBlock;
    ERepartitionMode repartitionMode = eRepartitionMultiResolution;
    std::size_t estimateSpaceMinObservations = 3;
    float estimateSpaceMinObservationAngle = 10.0f;
    double universePercentile = 0.999;
    int maxPtsPerVoxel = 6000000;
    bool meshingFromDepthMaps = true;
    bool estimateSpaceFromSfM = true;
    bool addLandmarksToTheDensePointCloud = false;
    bool saveRawDensePointCloud = false;
    bool colorizeOutput = false;
    float forceTEdgeDelta = 0.1f;
    unsigned int seed = 0;

    fuseCut::FuseParams fuseParams;

    po::options_description allParams("AliceVision meshing");

    po::options_description requiredParams("Required parameters");
    requiredParams.add_options()
        ("input,i", po::value<std::string>(&sfmDataFilename)->required(),
          "SfMData file.")
        ("output,o", po::value<std::string>(&outputDensePointCloud)->required(),
          "Output Dense SfMData file.")
        ("outputMesh,o", po::value<std::string>(&outputMesh)->required(),
          "Output mesh (OBJ file format).");

    po::options_description optionalParams("Optional parameters");
    optionalParams.add_options()
        ("depthMapsFolder", po::value<std::string>(&depthMapsFolder),
            "Input filtered depth maps folder.")
        ("maxInputPoints", po::value<int>(&fuseParams.maxInputPoints)->default_value(fuseParams.maxInputPoints),
            "Max input points loaded from images.")
        ("maxPoints", po::value<int>(&fuseParams.maxPoints)->default_value(fuseParams.maxPoints),
            "Max points at the end of the depth maps fusion.")
        ("maxPointsPerVoxel", po::value<int>(&maxPtsPerVoxel)->default_value(maxPtsPerVoxel),
            "Max points per voxel.")
        ("minStep", po::value<int>(&fuseParams.minStep)->default_value(fuseParams.minStep),
            "The step used to load depth values from depth maps is computed from maxInputPts. Here we define the minimal value for this step, "
            "so on small datasets we will not spend too much time at the beginning loading all depth values.")
        ("simFactor", po::value<float>(&fuseParams.simFactor)->default_value(fuseParams.simFactor),
            "simFactor")
        ("angleFactor", po::value<float>(&fuseParams.angleFactor)->default_value(fuseParams.angleFactor),
            "angleFactor")
        ("partitioning", po::value<EPartitioningMode>(&partitioningMode)->default_value(partitioningMode),
            "Partitioning: 'singleBlock' or 'auto'.")
        ("repartition", po::value<ERepartitionMode>(&repartitionMode)->default_value(repartitionMode),
            "Repartition: 'multiResolution' or 'regularGrid'.")
        ("estimateSpaceFromSfM", po::value<bool>(&estimateSpaceFromSfM)->default_value(estimateSpaceFromSfM),
            "Estimate the 3d space from the SfM.")
        ("addLandmarksToTheDensePointCloud", po::value<bool>(&addLandmarksToTheDensePointCloud)->default_value(addLandmarksToTheDensePointCloud),
            "Add SfM Landmarks into the dense point cloud (created from depth maps). If only the SfM is provided in input, SfM landmarks will be used regardless of this option.")
        ("colorizeOutput", po::value<bool>(&colorizeOutput)->default_value(colorizeOutput),
            "Whether to colorize output dense point cloud and mesh.");

    po::options_description advancedParams("Advanced parameters");
    advancedParams.add_options()
        ("universePercentile", po::value<double>(&universePercentile)->default_value(universePercentile),
            "universe percentile")
        ("estimateSpaceMinObservations", po::value<std::size_t>(&estimateSpaceMinObservations)->default_value(estimateSpaceMinObservations),
            "Minimum number of observations for SfM space estimation.")
        ("estimateSpaceMinObservationAngle", po::value<float>(&estimateSpaceMinObservationAngle)->default_value(estimateSpaceMinObservationAngle),
            "Minimum angle between two observations for SfM space estimation.")
        ("pixSizeMarginInitCoef", po::value<double>(&fuseParams.pixSizeMarginInitCoef)->default_value(fuseParams.pixSizeMarginInitCoef),
            "pixSizeMarginInitCoef")
        ("pixSizeMarginFinalCoef", po::value<double>(&fuseParams.pixSizeMarginFinalCoef)->default_value(fuseParams.pixSizeMarginFinalCoef),
            "pixSizeMarginFinalCoef")
        ("voteMarginFactor", po::value<float>(&fuseParams.voteMarginFactor)->default_value(fuseParams.voteMarginFactor),
            "voteMarginFactor")
        ("contributeMarginFactor", po::value<float>(&fuseParams.contributeMarginFactor)->default_value(fuseParams.contributeMarginFactor),
            "contributeMarginFactor")
        ("simGaussianSizeInit", po::value<float>(&fuseParams.simGaussianSizeInit)->default_value(fuseParams.simGaussianSizeInit),
            "simGaussianSizeInit")
        ("simGaussianSize", po::value<float>(&fuseParams.simGaussianSize)->default_value(fuseParams.simGaussianSize),
            "simGaussianSize")
        ("minAngleThreshold", po::value<double>(&fuseParams.minAngleThreshold)->default_value(fuseParams.minAngleThreshold),
            "minAngleThreshold")
        ("refineFuse", po::value<bool>(&fuseParams.refineFuse)->default_value(fuseParams.refineFuse),
            "refineFuse")
        ("saveRawDensePointCloud", po::value<bool>(&saveRawDensePointCloud)->default_value(saveRawDensePointCloud),
            "Save dense point cloud before cut and filtering.")
        ("forceTEdgeDelta", po::value<float>(&forceTEdgeDelta)->default_value(forceTEdgeDelta),
            "0 to disable force T edge in graphcut. Threshold for emptiness/fullness variation.")
        ("seed", po::value<unsigned int>(&seed)->default_value(seed),
         "Seed used in random processes. (0 to use a random seed)."); 

    po::options_description logParams("Log parameters");
    logParams.add_options()
      ("verboseLevel,v", po::value<std::string>(&verboseLevel)->default_value(verboseLevel),
        "verbosity level (fatal, error, warning, info, debug, trace).");

    allParams.add(requiredParams).add(optionalParams).add(advancedParams).add(logParams);

    po::variables_map vm;

    try
    {
      po::store(po::parse_command_line(argc, argv, allParams), vm);

      if(vm.count("help") || (argc == 1))
      {
        ALICEVISION_COUT(allParams);
        return EXIT_SUCCESS;
      }

      po::notify(vm);
    }
    catch(boost::program_options::required_option& e)
    {
      ALICEVISION_CERR("ERROR: " << e.what() << std::endl);
      ALICEVISION_COUT("Usage:\n\n" << allParams);
      return EXIT_FAILURE;
    }
    catch(boost::program_options::error& e)
    {
      ALICEVISION_CERR("ERROR: " << e.what() << std::endl);
      ALICEVISION_COUT("Usage:\n\n" << allParams);
      return EXIT_FAILURE;
    }

    ALICEVISION_COUT("Program called with the following parameters:");
    ALICEVISION_COUT(vm);

    // set verbose level
    system::Logger::get()->setLogLevel(verboseLevel);

    if(depthMapsFolder.empty())
    {
      if(depthMapsFolder.empty() &&
         repartitionMode == eRepartitionMultiResolution &&
         partitioningMode == ePartitioningSingleBlock)
      {
        meshingFromDepthMaps = false;
        addLandmarksToTheDensePointCloud = true;
      }
      else
      {
        ALICEVISION_LOG_ERROR("Invalid input options:\n"
                              "- Meshing from depth maps require --depthMapsFolder option.\n"
                              "- Meshing from SfM require option --partitioning set to 'singleBlock' and option --repartition set to 'multiResolution'.");
        return EXIT_FAILURE;
      }
    }

    // read the input SfM scene
    sfmData::SfMData sfmData;
    if(!sfmDataIO::Load(sfmData, sfmDataFilename, sfmDataIO::ESfMData::ALL))
    {
      ALICEVISION_LOG_ERROR("The input SfMData file '" << sfmDataFilename << "' cannot be read.");
      return EXIT_FAILURE;
    }

    // initialization
    mvsUtils::MultiViewParams mp(sfmData, "", "", depthMapsFolder, meshingFromDepthMaps);

    mp.userParams.put("LargeScale.universePercentile", universePercentile);
    mp.userParams.put("delaunaycut.forceTEdgeDelta", forceTEdgeDelta);
    mp.userParams.put("delaunaycut.seed", seed);

    int ocTreeDim = mp.userParams.get<int>("LargeScale.gridLevel0", 1024);
    const auto baseDir = mp.userParams.get<std::string>("LargeScale.baseDirName", "root01024");

    fs::path outDirectory = fs::path(outputMesh).parent_path();
    if(!fs::is_directory(outDirectory))
        fs::create_directory(outDirectory);

    fs::path tmpDirectory = outDirectory / "tmp";

    ALICEVISION_LOG_WARNING("repartitionMode: " << repartitionMode);
    ALICEVISION_LOG_WARNING("partitioningMode: " << partitioningMode);

    mesh::Mesh* mesh = nullptr;
    StaticVector<StaticVector<int>> ptsCams;

    switch(repartitionMode)
    {
        case eRepartitionMultiResolution:
        {
            switch(partitioningMode)
            {
                case ePartitioningAuto:
                {
                    throw std::invalid_argument("Meshing mode: 'multiResolution', partitioning: 'auto' is not yet implemented.");
                }
                case ePartitioningSingleBlock:
                {
                    ALICEVISION_LOG_INFO("Meshing mode: multi-resolution, partitioning: single block.");
                    std::array<Point3d, 8> hexah;

                    float minPixSize;
                    fuseCut::Fuser fs(&mp);

                    if(meshingFromDepthMaps && !estimateSpaceFromSfM)
                      fs.divideSpaceFromDepthMaps(&hexah[0], minPixSize);
                    else
                      fs.divideSpaceFromSfM(sfmData, &hexah[0], estimateSpaceMinObservations, estimateSpaceMinObservationAngle);

                    StaticVector<int> cams;
                    if(meshingFromDepthMaps)
                    {
                      cams = mp.findCamsWhichIntersectsHexahedron(&hexah[0]);
                    }
                    else
                    {
                      cams.resize(mp.getNbCameras());
                      for(int i = 0; i < cams.size(); ++i)
                          cams[i] = i;
                    }

                    if(cams.empty())
                        throw std::logic_error("No camera to make the reconstruction");
                    
                    fuseCut::DelaunayGraphCut delaunayGC(&mp);
                    delaunayGC.createDensePointCloud(&hexah[0], cams, addLandmarksToTheDensePointCloud ? &sfmData : nullptr, meshingFromDepthMaps ? &fuseParams : nullptr);
                    if(saveRawDensePointCloud)
                    {
                      ALICEVISION_LOG_INFO("Save dense point cloud before cut and filtering.");
                      StaticVector<StaticVector<int>> ptsCams;
                      delaunayGC.createPtsCams(ptsCams);
                      sfmData::SfMData densePointCloud;
                      createDenseSfMData(sfmData, mp, delaunayGC._verticesCoords, ptsCams, densePointCloud);
                      removeLandmarksWithoutObservations(densePointCloud);
                      if(colorizeOutput)
                        sfmData::colorizeTracks(densePointCloud);
                      sfmDataIO::Save(densePointCloud, (outDirectory/"densePointCloud_raw.abc").string(), sfmDataIO::ESfMData::ALL_DENSE);
                    }

                    delaunayGC.createGraphCut(&hexah[0], cams, outDirectory.string()+"/", outDirectory.string()+"/SpaceCamsTracks/", false);
                    delaunayGC.graphCutPostProcessing();
                    mesh = delaunayGC.createMesh();
                    delaunayGC.createPtsCams(ptsCams);
                    mesh::meshPostProcessing(mesh, ptsCams, mp, outDirectory.string()+"/", nullptr, &hexah[0]);

                    break;
                }
                case ePartitioningUndefined:
                default:
                    throw std::invalid_argument("Partitioning mode is not defined");
            }
            break;
        }
        case eRepartitionUndefined:
        default:
            throw std::invalid_argument("Repartition mode is not defined");
    }

    // Generate output files: 
    // - dense point-cloud with observations as sfmData
    // - mesh as .obj

    if(mesh == nullptr || mesh->pts.empty() || mesh->tris.empty())
      throw std::runtime_error("No valid mesh was generated.");

    if(ptsCams.empty())
      throw std::runtime_error("Points visibilities data has not been initialized.");

    sfmData::SfMData densePointCloud;
    createDenseSfMData(sfmData, mp, mesh->pts.getData(), ptsCams, densePointCloud);

    if(colorizeOutput)
    {
      sfmData::colorizeTracks(densePointCloud);
      // colorize output mesh before landmarks filtering
      // to have a 1:1 mapping between points and mesh vertices
      const auto& landmarks = densePointCloud.getLandmarks();
      std::vector<rgb>& colors = mesh->colors();
      colors.resize(mesh->pts.size(), {0, 0, 0});
      for(std::size_t i = 0; i < mesh->pts.size(); ++i)
      {
        const auto& c = landmarks.at(i).rgb;
        colors[i] = {c.r(), c.g(), c.b()};
      }
    }

    removeLandmarksWithoutObservations(densePointCloud);
    ALICEVISION_LOG_INFO("Save dense point cloud.");
    sfmDataIO::Save(densePointCloud, outputDensePointCloud, sfmDataIO::ESfMData::ALL_DENSE);

    ALICEVISION_LOG_INFO("Save obj mesh file.");
    mesh->saveToObj(outputMesh);
    delete mesh;


    ALICEVISION_LOG_INFO("Task done in (s): " + std::to_string(timer.elapsed()));
    return EXIT_SUCCESS;
}
