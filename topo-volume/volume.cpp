#include <iostream>
#include <cstring>
#include <algorithm>
#include <limits>
#include <glm/glm.hpp>
#include <vtkType.h>
#include "glt/util.h"
#include "volume.h"

static const std::array<float, 42> CUBE_STRIP = {
	1, 1, 0,
	0, 1, 0,
	1, 1, 1,
	0, 1, 1,
	0, 0, 1,
	0, 1, 0,
	0, 0, 0,
	1, 1, 0,
	1, 0, 0,
	1, 1, 1,
	1, 0, 1,
	0, 0, 1,
	1, 0, 0,
	0, 0, 0
};

static void vtk_type_to_gl(const int vtk, GLenum &gl_internal, GLenum &gl_type) {
	switch (vtk) {
		case VTK_CHAR:
		case VTK_UNSIGNED_CHAR:
			gl_internal = GL_R8;
			gl_type = GL_UNSIGNED_BYTE;
			break;
		case VTK_FLOAT:
			gl_internal = GL_R32F;
			gl_type = GL_FLOAT;
			break;
		default:
			throw std::runtime_error("Unsupported VTK data type!");
	}
}

Volume::Volume(vtkImageData *vol)
	: vol_data(vol), transform_dirty(true), translation(0), scaling(1)
{
	vtk_type_to_gl(vol->GetScalarType(), internal_format, format);
	for (size_t i = 0; i < 3; ++i) {
		dims[i] = vol->GetDimensions()[i];
		vol_render_size[i] = vol->GetSpacing()[i];
	}
	// Center the volume in the world
	translate(glm::vec3(vol_render_size[0], vol_render_size[1], vol_render_size[2])
			* glm::vec3(-0.5));
	build_histogram();
}
Volume::~Volume(){
	if (allocator){
		allocator->free(cube_buf);
		allocator->free(vol_props);
		glDeleteVertexArrays(1, &vao);
		glDeleteTextures(1, &texture);
		glDeleteProgram(shader);
	}
}
void Volume::translate(const glm::vec3 &v){
	translation += v;
	transform_dirty = true;
}
void Volume::scale(const glm::vec3 &v){
	scaling *= v;
	transform_dirty = true;
}
void Volume::rotate(const glm::quat &r){
	rotation = r * rotation;
	transform_dirty = true;
}
void Volume::set_base_matrix(const glm::mat4 &m){
	base_matrix = m;
	transform_dirty = true;
}
void Volume::render(std::shared_ptr<glt::BufferAllocator> &buf_allocator) {
	// We need to apply the inverse volume transform to the eye to get it in the volume's space
	glm::mat4 vol_transform = glm::translate(translation) * glm::mat4_cast(rotation)
		* glm::scale(scaling * vol_render_size) * base_matrix;
	// Setup shaders, vao and volume texture
	if (!allocator){
		glGenVertexArrays(1, &vao);
		glBindVertexArray(vao);
		allocator = buf_allocator;
		// Setup our cube tri strip to draw the bounds of the volume to raycast against
		cube_buf = buf_allocator->alloc(sizeof(float) * CUBE_STRIP.size());
		{
			float *buf = reinterpret_cast<float*>(cube_buf.map(GL_ARRAY_BUFFER,
						GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_WRITE_BIT));
			for (size_t i = 0; i < CUBE_STRIP.size(); ++i){
				buf[i] = CUBE_STRIP[i];
			}
			cube_buf.unmap(GL_ARRAY_BUFFER);
		}
		glEnableVertexAttribArray(0);
		glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, (void*)cube_buf.offset);

		vol_props = buf_allocator->alloc(2 * sizeof(glm::mat4) + sizeof(glm::vec4) + sizeof(glm::vec2),
			glt::BufAlignment::UNIFORM_BUFFER);
		{
			char *buf = reinterpret_cast<char*>(vol_props.map(GL_UNIFORM_BUFFER,
						GL_MAP_INVALIDATE_RANGE_BIT | GL_MAP_WRITE_BIT));
			glm::mat4 *mats = reinterpret_cast<glm::mat4*>(buf);
			glm::vec4 *vecs = reinterpret_cast<glm::vec4*>(buf + 2 * sizeof(glm::mat4));
			glm::vec2 *scale_bias = reinterpret_cast<glm::vec2*>(buf + 2 * sizeof(glm::mat4) + sizeof(glm::vec4));
			mats[0] = vol_transform;
			mats[1] = glm::inverse(mats[0]);
			vecs[0] = glm::vec4{static_cast<float>(dims[0]), static_cast<float>(dims[1]),
				static_cast<float>(dims[2]), 0};
			// Set scaling and bias to scale the volume values
			*scale_bias = glm::vec2{1.f / (vol_max - vol_min), -vol_min};

			// TODO: Again how will this interact with multiple folks doing this?
			glBindBufferRange(GL_UNIFORM_BUFFER, 1, vol_props.buffer, vol_props.offset, vol_props.size);
			vol_props.unmap(GL_UNIFORM_BUFFER);
			transform_dirty = false;
		}

		glGenTextures(1, &texture);

		// TODO: If drawing multiple volumes they can all share the same program
		const std::string resource_path = glt::get_resource_path();
		shader = glt::load_program({std::make_pair(GL_VERTEX_SHADER, resource_path + "vol_vert.glsl"),
				std::make_pair(GL_FRAGMENT_SHADER, resource_path + "vol_frag.glsl")});
		glUseProgram(shader);
		// TODO: how does this interact with having multiple volumes? should we just
		// have GL4.5 as a hard requirement for DSA? Can I get 4.5 on my laptop?
		glUniform1i(glGetUniformLocation(shader, "volume"), 1);
		glUniform1i(glGetUniformLocation(shader, "palette"), 2);
	}
	// Upload the volume data, it's changed
	if (vol_data){
		/*
		// TODO: Is there sparse textures for 3d textures? How would it work streaming idx data
		// up instead of re-calling teximage3d and creating/destroying the texture each time?
		// Allocate storage space for the texture
		glTexStorage3D(GL_TEXTURE_3D, 1, internal_format, dims[0], dims[1], dims[2]);
		// Upload our data
		glTexSubImage3D(GL_TEXTURE_3D, 0, 0, 0, 0, dims[0], dims[1], dims[2], GL_RED,
		format, static_cast<const void*>(data.data()));
		*/
		glActiveTexture(GL_TEXTURE1);
		glBindTexture(GL_TEXTURE_3D, texture);
		glTexImage3D(GL_TEXTURE_3D, 0, internal_format, dims[0], dims[1], dims[2], 0, GL_RED,
				format, static_cast<const void*>(vol_data->GetScalarPointer()));
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
		glTexParameteri(GL_TEXTURE_3D, GL_TEXTURE_WRAP_R, GL_CLAMP_TO_BORDER);
		// We've uploaded the data and don't need a reference any more
		vol_data = nullptr;
		// We're changing the volume so also update the volume properties buffer
		{
			char *buf = reinterpret_cast<char*>(vol_props.map(GL_UNIFORM_BUFFER, GL_MAP_WRITE_BIT));
			glm::mat4 *mats = reinterpret_cast<glm::mat4*>(buf);
			glm::vec4 *vecs = reinterpret_cast<glm::vec4*>(buf + 2 * sizeof(glm::mat4));
			glm::vec2 *scale_bias = reinterpret_cast<glm::vec2*>(buf + 2 * sizeof(glm::mat4) + sizeof(glm::vec4));
			mats[0] = vol_transform;
			mats[1] = glm::inverse(mats[0]);
			vecs[0] = glm::vec4{ static_cast<float>(dims[0]), static_cast<float>(dims[1]),
				static_cast<float>(dims[2]), 0 };

			// Set scaling and bias to scale the volume values
			*scale_bias = glm::vec2{1.f / (vol_max - vol_min), -vol_min};

			vol_props.unmap(GL_UNIFORM_BUFFER);
			transform_dirty = false;
		}
	}
	if (transform_dirty){
		char *buf = reinterpret_cast<char*>(vol_props.map(GL_UNIFORM_BUFFER, GL_MAP_WRITE_BIT));
		glm::mat4 *mats = reinterpret_cast<glm::mat4*>(buf);
		mats[0] = vol_transform;
		mats[1] = glm::inverse(mats[0]);
		vol_props.unmap(GL_UNIFORM_BUFFER);
		transform_dirty = false;
	}
	glEnable(GL_CULL_FACE);
	glCullFace(GL_FRONT);
	glEnable(GL_BLEND);
	glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

	glBindBufferRange(GL_UNIFORM_BUFFER, 1, vol_props.buffer, vol_props.offset, vol_props.size);
	glActiveTexture(GL_TEXTURE1);
	glBindTexture(GL_TEXTURE_3D, texture);
	glUseProgram(shader);
	glBindVertexArray(vao);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, CUBE_STRIP.size() / 3);

	glCullFace(GL_BACK);
	glDisable(GL_CULL_FACE);
}
void Volume::build_histogram(){
	// Find scale & bias for the volume data
	if (internal_format == GL_R32F && format == GL_FLOAT){
		// Find the min/max values in the volume
		vol_min = vol_data->GetScalarRange()[0];
		vol_max = vol_data->GetScalarRange()[1];
		std::cout << "Found min max = {" << vol_min << ", " << vol_max << "}\n";
	}

	// For non f32 or f16 textures GL will normalize for us, given that we've done
	// the proper range correction above if needed (e.g. for R16)
	if (internal_format == GL_R8) {
		std::cout << "Setting gl min max to {0, 1} for R8 data\n";
		vol_min = 0;
		vol_max = 1;
	}

	// Build the histogram for the data
	histogram.clear();
	histogram.resize(100, 0);
	const size_t num_voxels = dims[0] * dims[1] * dims[2];
	if (format == GL_FLOAT){
		float *data_ptr = reinterpret_cast<float*>(vol_data->GetScalarPointer());
		for (size_t i = 0; i < num_voxels; ++i){
			size_t bin = static_cast<size_t>((data_ptr[i] - vol_min) / (vol_max - vol_min) * histogram.size());
			bin = glm::clamp(bin, size_t{0}, histogram.size() - 1);
			++histogram[bin];
		}
	} else if (format == GL_UNSIGNED_BYTE){
		uint8_t *data_ptr = reinterpret_cast<uint8_t*>(vol_data->GetScalarPointer());
		for (size_t i = 0; i < num_voxels; ++i){
			size_t bin = static_cast<size_t>((data_ptr[i] - vol_min) / (vol_max - vol_min) * histogram.size());
			bin = glm::clamp(bin, size_t{0}, histogram.size() - 1);
			++histogram[bin];
		}
	}
}
