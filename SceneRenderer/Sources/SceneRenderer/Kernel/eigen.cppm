module;

#include <Eigen/Dense>
#include <Eigen/Geometry>

export module eigen;

export namespace Eigen
{

// core templates
using Eigen::Array;
using Eigen::DenseBase;
using Eigen::Matrix;
using Eigen::MatrixBase;
using Eigen::PlainObjectBase;

// fixed-size float/double vectors
using Eigen::Vector2d;
using Eigen::Vector2f;
using Eigen::Vector2i;
using Eigen::Vector3d;
using Eigen::Vector3f;
using Eigen::Vector3i;
using Eigen::Vector4d;
using Eigen::Vector4f;
using Eigen::Vector4i;

// dynamic-size vectors
using Eigen::VectorXd;
using Eigen::VectorXf;
using Eigen::VectorXi;

// row vectors
using Eigen::RowVector2d;
using Eigen::RowVector2f;
using Eigen::RowVector3d;
using Eigen::RowVector3f;
using Eigen::RowVector4d;
using Eigen::RowVector4f;

// fixed-size matrices
using Eigen::Matrix2d;
using Eigen::Matrix2f;
using Eigen::Matrix3d;
using Eigen::Matrix3f;
using Eigen::Matrix4d;
using Eigen::Matrix4f;

// dynamic-size matrices
using Eigen::MatrixXd;
using Eigen::MatrixXf;
using Eigen::MatrixXi;

// geometry: rotations / transforms
using Eigen::AngleAxis;
using Eigen::AngleAxisd;
using Eigen::AngleAxisf;
using Eigen::Quaternion;
using Eigen::Quaterniond;
using Eigen::Quaternionf;
using Eigen::Rotation2D;
using Eigen::Rotation2Dd;
using Eigen::Rotation2Df;
using Eigen::Scaling;
using Eigen::Translation;
using Eigen::Translation2d;
using Eigen::Translation2f;
using Eigen::Translation3d;
using Eigen::Translation3f;
using Eigen::UniformScaling;

// affine / isometry / projective transforms
using Eigen::Affine2d;
using Eigen::Affine2f;
using Eigen::Affine3d;
using Eigen::Affine3f;
using Eigen::Isometry2d;
using Eigen::Isometry2f;
using Eigen::Isometry3d;
using Eigen::Isometry3f;
using Eigen::Projective2d;
using Eigen::Projective2f;
using Eigen::Projective3d;
using Eigen::Projective3f;
using Eigen::Transform;

// views / references
using Eigen::InnerStride;
using Eigen::Map;
using Eigen::OuterStride;
using Eigen::Ref;
using Eigen::Stride;

// enum tags (StorageOptions, AlignmentType, TransformTraits)
using Eigen::Affine;
using Eigen::AutoAlign;
using Eigen::ColMajor;
using Eigen::DontAlign;
using Eigen::Index;
using Eigen::Isometry;
using Eigen::Projective;
using Eigen::RowMajor;

} // namespace Eigen
