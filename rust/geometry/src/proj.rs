//! Projection ported from `proj.cpp`.
//!
//! The engine projects 3D table coordinates to the 2D sprite space with a fixed
//! 4x3 matrix plus a scale/offset pair, and converts sprite depth to the 16-bit
//! z-buffer value. It is called for every sprite, every frame, so it sits next to
//! the maths kernel as hot pure code.
//!
//! As with the rest of this crate, the state stays in C++ (the `proj` statics)
//! and is passed in: the functions here are pure, which keeps the port testable
//! against the original without a second copy of the engine's globals.

/// 4-wide row of the projection matrix; matches C++ `vector_type4`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Vector4 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

/// Projection matrix, row major; matches C++ `mat4_row_major`.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct Mat4 {
    pub row0: Vector4,
    pub row1: Vector4,
    pub row2: Vector4,
    pub row3: Vector4,
}

/// The `proj` scalars a projection call needs.
#[repr(C)]
#[derive(Clone, Copy, Debug, Default, PartialEq)]
pub struct ProjectionParams {
    pub d: f32,
    pub center_x: f32,
    pub center_y: f32,
    pub z_scaler: f32,
    pub z_min: f32,
    pub z_max: f32,
}

/// Multiply a 3D vector by the projection matrix.
///
/// Mirrors `proj::matrix_vector_multiply`, including the order of the additions:
/// the matrix is applied to a homogeneous point with w = 1.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_proj_matrix_vector_multiply(
    matrix: *const Mat4,
    vector: *const crate::Vector3,
) -> crate::Vector3 {
    // SAFETY: the C++ wrapper passes valid, initialized values.
    let matrix = unsafe { *matrix };
    let vector = unsafe { *vector };

    crate::Vector3 {
        x: vector.z * matrix.row0.z
            + vector.y * matrix.row0.y
            + vector.x * matrix.row0.x
            + matrix.row0.w,
        y: vector.z * matrix.row1.z
            + vector.y * matrix.row1.y
            + vector.x * matrix.row1.x
            + matrix.row1.w,
        z: vector.z * matrix.row2.z
            + vector.y * matrix.row2.y
            + vector.x * matrix.row2.x
            + matrix.row2.w,
    }
}

/// Projected depth of a 3D point, used to pick the sprite and the z-buffer slot.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_proj_z_distance(
    matrix: *const Mat4,
    vector: *const crate::Vector3,
) -> f32 {
    // SAFETY: the C++ wrapper passes valid pointers.
    let projected = unsafe { scp_proj_matrix_vector_multiply(matrix, vector) };
    // SAFETY: `projected` is a valid local value.
    unsafe { crate::scp_geometry_magnitude(&projected) }
}

/// Project a 3D point to 2D sprite coordinates.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_proj_xform_to_2d(
    matrix: *const Mat4,
    params: *const ProjectionParams,
    vector: *const crate::Vector3,
    output: *mut crate::Vector2i,
) {
    // SAFETY: the C++ wrapper passes valid pointers and writable output storage.
    let params = unsafe { *params };
    let projected = unsafe { scp_proj_matrix_vector_multiply(matrix, vector) };

    let coefficient = if projected.z == 0.0 {
        999_999.88
    } else {
        params.d / projected.z
    };

    let x = (projected.x * coefficient + params.center_x) as i32;
    let y = (projected.y * coefficient + params.center_y) as i32;

    // SAFETY: the caller passes writable storage for one Vector2i.
    unsafe { *output = crate::Vector2i { x, y } };
}

/// Reverse-project a 2D sprite coordinate back to the table plane (z = 0).
///
/// Mirrors `proj::ReverseXForm`, including its use of the matrix rows rather than
/// the hard-coded constants in its comment.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_proj_reverse_xform(
    matrix: *const Mat4,
    params: *const ProjectionParams,
    vector: *const crate::Vector2i,
) -> crate::Vector3 {
    // SAFETY: the C++ wrapper passes valid pointers.
    let matrix = unsafe { *matrix };
    let params = unsafe { *params };
    let vector = unsafe { *vector };

    let a = matrix.row1.y;
    let b = matrix.row1.z;
    let f = matrix.row1.w;
    let g = matrix.row2.w;
    let d = params.d;
    let x2 = (vector.x as f32 - params.center_x) / d;
    let y2 = (vector.y as f32 - params.center_y) / d;
    let z0 = 0.0;

    let y0 = (y2 * (a * z0 + g) - b * z0 - f) / (a + b * y2);
    let x0 = x2 * (a * z0 - b * y0 + g);
    crate::Vector3 {
        x: x0,
        y: y0,
        z: z0,
    }
}

/// Depth to 16-bit z-buffer value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn scp_proj_normalize_depth(
    params: *const ProjectionParams,
    depth: f32,
) -> u16 {
    // SAFETY: the C++ wrapper passes a valid params pointer.
    let params = unsafe { *params };

    if depth >= params.z_min {
        let scaled = (depth - params.z_min) * params.z_scaler;
        if scaled <= params.z_max {
            // The C++ cast of an out-of-range float to `uint16_t` is undefined,
            // but every compiler this engine ships with converts through a
            // 32-bit integer and keeps the low 16 bits. Reproduce that rather
            // than saturate, so a depth the original wraps stays wrapped.
            return (scaled as i32) as u16;
        }
        return 0xffff;
    }
    0
}

#[cfg(test)]
mod tests {
    use super::*;

    fn matrix() -> Mat4 {
        // The table's fixed perspective matrix (see proj::ReverseXForm).
        Mat4 {
            row0: Vector4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
            row1: Vector4 {
                x: 0.0,
                y: -0.913_545,
                z: 0.406_737,
                w: 3.791_398,
            },
            row2: Vector4 {
                x: 0.0,
                y: -0.406_737,
                z: -0.913_545,
                w: 24.675_402,
            },
            row3: Vector4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0,
            },
        }
    }

    fn params() -> ProjectionParams {
        ProjectionParams {
            d: 500.0,
            center_x: 400.0,
            center_y: 300.0,
            z_scaler: 0.001,
            z_min: 0.0,
            z_max: 4_294_967_296.0 / 0.001,
        }
    }

    #[test]
    fn matrix_multiply_applies_translation_row() {
        let m = matrix();
        let v = crate::Vector3 {
            x: 0.0,
            y: 0.0,
            z: 0.0,
        };
        // SAFETY: valid pointers to local values.
        let projected = unsafe { scp_proj_matrix_vector_multiply(&m, &v) };
        assert_eq!(projected.x, 0.0);
        assert_eq!(projected.y, 3.791_398);
        assert_eq!(projected.z, 24.675_402);
    }

    #[test]
    fn projection_and_reverse_projection_round_trip_the_table_plane() {
        let m = matrix();
        let p = params();
        let point = crate::Vector3 {
            x: 120.0,
            y: -80.0,
            z: 0.0,
        };

        let mut screen = crate::Vector2i::default();
        // SAFETY: valid pointers to local values.
        unsafe { scp_proj_xform_to_2d(&m, &p, &point, &mut screen) };
        // SAFETY: valid pointers to local values.
        let back = unsafe { scp_proj_reverse_xform(&m, &p, &screen) };

        // The round trip is exact up to the integer truncation of the screen
        // coordinate, so allow one pixel of slack.
        assert!(
            (back.x - point.x).abs() <= 2.0,
            "x {} vs {}",
            back.x,
            point.x
        );
        assert!(
            (back.y - point.y).abs() <= 2.0,
            "y {} vs {}",
            back.y,
            point.y
        );
        assert_eq!(back.z, 0.0);
    }

    #[test]
    fn depth_normalization_maps_the_visible_range() {
        let mut p = params();
        p.z_scaler = 1000.0;
        // Below z_min the engine stores 0 ("no depth").
        assert_eq!(unsafe { scp_proj_normalize_depth(&p, -1.0) }, 0);
        assert_eq!(unsafe { scp_proj_normalize_depth(&p, 10.0) }, 10_000);
        assert_eq!(unsafe { scp_proj_normalize_depth(&p, 60.0) }, 60_000);
        // Wraps like the original cast rather than saturating.
        assert_eq!(
            unsafe { scp_proj_normalize_depth(&p, 70.0) },
            (70_000i32) as u16
        );
    }
}
