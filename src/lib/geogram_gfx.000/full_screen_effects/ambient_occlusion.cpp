/*
 *  Copyright (c) 2012-2014, Bruno Levy
 *  All rights reserved.
 *
 *  Redistribution and use in source and binary forms, with or without
 *  modification, are permitted provided that the following conditions are met:
 *
 *  * Redistributions of source code must retain the above copyright notice,
 *  this list of conditions and the following disclaimer.
 *  * Redistributions in binary form must reproduce the above copyright notice,
 *  this list of conditions and the following disclaimer in the documentation
 *  and/or other materials provided with the distribution.
 *  * Neither the name of the ALICE Project-Team nor the names of its
 *  contributors may be used to endorse or promote products derived from this
 *  software without specific prior written permission.
 * 
 *  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 *  AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 *  IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 *  ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
 *  LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 *  CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 *  SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 *  INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 *  CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 *  ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 *  POSSIBILITY OF SUCH DAMAGE.
 *
 *  If you modify this software, you should include a notice giving the
 *  name of the person performing the modification, the date of modification,
 *  and the reason for such modification.
 *
 *  Contact: Bruno Levy
 *
 *     Bruno.Levy@inria.fr
 *     http://www.loria.fr/~levy
 *
 *     ALICE Project
 *     LORIA, INRIA Lorraine, 
 *     Campus Scientifique, BP 239
 *     54506 VANDOEUVRE LES NANCY CEDEX 
 *     FRANCE
 *
 */

#include <geogram_gfx/full_screen_effects/ambient_occlusion.h>
#include <geogram_gfx/basic/GLSL.h>
#include <geogram/basic/logger.h>

namespace {
    using namespace GEO;
    static const index_t REDUCTION = 4;
    static const index_t R_WIDTH  = 32;
    static const index_t R_HEIGHT = 32;
}

namespace GEO {

    AmbientOcclusionImpl::AmbientOcclusionImpl() {
        lightness_ = 10;
        contrast_ = 10;
        blur_width_ = 2;
        nb_directions_ = 7;

        SSAO_program_ = 0;
        blur_program_ = 0;        
        depth_tex_ = 0;
        random_tex_ = 0;
    }

    AmbientOcclusionImpl::~AmbientOcclusionImpl() {
        glDeleteTextures(1, &depth_tex_);
        depth_tex_ = 0;
        glDeleteTextures(1, &random_tex_);
        random_tex_ = 0;
        if (SSAO_program_ != 0) {
            glDeleteProgram(SSAO_program_);
        }
        if (blur_program_ != 0) {
            glDeleteProgram(blur_program_);
        }
    }

    double AmbientOcclusionImpl::required_GLSL_version() const {
#ifdef GEO_OS_EMSCRIPTEN
	return 1.0;
#else	    
        return 1.3;
#endif	
    }
    
    void AmbientOcclusionImpl::pre_render(index_t w, index_t h) {
        FullScreenEffectImpl::pre_render(w,h);
    }

    void AmbientOcclusionImpl::post_render() {
#ifndef GEO_OS_EMSCRIPTEN
	if(!core_profile_) {
	    glDisable(GL_CLIP_PLANE0);
	}
#endif       
        get_proj_inv();
#ifndef GEO_OS_EMSCRIPTEN       
	if(!core_profile_) {
	    glDisable(GL_LIGHTING);
	}
#endif       
        copy_depth_buffer_to_texture();
        copy_depth_texture_to_FBO();
        compute_SSAO();
        blur();
        display_final_texture();
	reset_alpha();
        FullScreenEffectImpl::post_render();        
    }

    void AmbientOcclusionImpl::update() {
        if(!OK()) {
            return;
        }
        if(SSAO_program_ != 0) {
            GLSL::set_program_uniform_by_name(
                SSAO_program_, "shadows_gamma", float(contrast_) / 10.0f
            );
            GLSL::set_program_uniform_by_name(
                SSAO_program_, "shadows_intensity", float(lightness_) / 10.0f
            );
            GLSL::set_program_uniform_by_name(
                SSAO_program_, "depth_cueing", 0.0f
            );
            GLSL::set_program_uniform_by_name(
                SSAO_program_, "nb_directions", float(nb_directions_)
            );
        }
        if(blur_program_ != 0) {
            GLSL::set_program_uniform_by_name(
                blur_program_, "blur_width", float(blur_width_)
            );
        }
    }
    
    void AmbientOcclusionImpl::initialize(index_t w, index_t h) {
        FullScreenEffectImpl::initialize(w,h);
        if(!OK()) {
            return;
        }

        glGenTextures(1, &depth_tex_);
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        
        // previously: const GLint internal_format = GL_LUMINANCE_FLOAT32_ATI;
        // Now deprecated, we use "red-only" texture format with floats
        // This requires using texture swizzle to copy the red component into
        // the other ones when looking-up the texture
        // (see display_final_texture()).
#ifdef GEO_OS_EMSCRIPTEN
        const GLint internal_format = GL_RGBA;       
#else
        const GLint internal_format = GL_R32F;
#endif       
        if(
            !reduced_depth_FBO_.initialize(
                width()  / REDUCTION,
                height() / REDUCTION,
                false, internal_format
             )
        ) {
            Logger::err("SSAO")
                << "Reduced depth FBO is not initialized" << std::endl;
        }

	// Shader sources are embedded in source code,
	// Initial sourcecode is in:
	// geogram_gfx/GLUP/shaders/fullscreen
	SSAO_program_ = GLSL::compile_program_with_includes_no_link(
	    this,
	    "//stage GL_VERTEX_SHADER\n"
	    "//import <fullscreen/vertex_shader.h>\n",
	    "//stage GL_FRAGMENT_SHADER\n"
	    "//import <fullscreen/ambient_occlusion_fragment_shader.h>\n"                
	);
	
	
        glBindAttribLocation(SSAO_program_, 0, "vertex_in");
        glBindAttribLocation(SSAO_program_, 1, "tex_coord_in");
        GLSL::link_program(SSAO_program_);
        
        GLSL::set_program_uniform_by_name(
            SSAO_program_, "depth_texture", 0
        );
        GLSL::set_program_uniform_by_name(
            SSAO_program_, "reduced_depth_texture", 1
        );
        GLSL::set_program_uniform_by_name(
            SSAO_program_, "random_texture", 2
        );
        proj_inv_loc_ = glGetUniformLocation(SSAO_program_, "mat_proj_inv");
        if( !blur_1_.initialize(
		width(), height(), false, internal_format)
	) {
            Logger::err("SSAO")
                << "blur_1_ FBO is not initialized" << std::endl;
        }
        if( !blur_2_.initialize(
		width(), height(), false, internal_format
	   )
	) {
            Logger::err("SSAO")
                << "blur_2_ FBO is not initialized" << std::endl;
        }


	// Shader sources are embedded in source code,
	// Initial sourcecode is in:
	// geogram_gfx/GLUP/shaders/fullscreen
	blur_program_ = GLSL::compile_program_with_includes_no_link(
	    this,
	    "//stage GL_VERTEX_SHADER\n"
	    "//import <fullscreen/vertex_shader.h>\n",
	    "//stage GL_FRAGMENT_SHADER\n"
	    "//import <fullscreen/depth_dependent_blur_fragment_shader.h>\n"                
	);

	
        glBindAttribLocation(blur_program_, 0, "vertex_in");
        glBindAttribLocation(blur_program_, 1, "tex_coord_in");
        GLSL::link_program(blur_program_);
        create_random_tex();
        
        GLSL::set_program_uniform_by_name(blur_program_, "texture2D", 0);
        GLSL::set_program_uniform_by_name(blur_program_, "depth_texture", 1);
        
        update();
    }

    void AmbientOcclusionImpl::resize(index_t width, index_t height) {
        blur_1_.resize(width, height);
        blur_2_.resize(width, height);
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        glTexImage2D(
            GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT,
            GLsizei(width), GLsizei(height), 0,
            GL_DEPTH_COMPONENT, GL_UNSIGNED_BYTE, 0
        );
        glBindTexture(GL_TEXTURE_2D, 0);
        reduced_depth_FBO_.resize(width / REDUCTION, height / REDUCTION);
        FullScreenEffectImpl::resize(width, height);
    }


    void AmbientOcclusionImpl::create_random_tex() {
        glGenTextures(1, &random_tex_);
        glBindTexture(GL_TEXTURE_2D, random_tex_);
        static Memory::byte tex_buff[R_WIDTH * R_HEIGHT];
        for (unsigned int i = 0; i < R_WIDTH * R_HEIGHT; i++) {
            tex_buff[i] = Memory::byte(Numeric::random_int32() & 255);
        }
#ifdef GEO_OS_EMSCRIPTEN
#define GL_RED GL_LUMINANCE       
#endif       
        glTexImage2D(
            GL_TEXTURE_2D, 0, 
	    GL_RED,
            R_WIDTH, R_HEIGHT, 0, 
	    GL_RED,
            GL_UNSIGNED_BYTE, tex_buff
        );
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    void AmbientOcclusionImpl::copy_depth_buffer_to_texture() {
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        glCopyTexImage2D(
            GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT, 0, 0,
            GLsizei(width()), GLsizei(height()), 0
        );
    }

    void AmbientOcclusionImpl::copy_depth_texture_to_FBO() {
        reduced_depth_FBO_.bind_as_framebuffer();
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        draw_unit_textured_quad();
        reduced_depth_FBO_.unbind();
    }

    void AmbientOcclusionImpl::display_final_texture() {
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_ZERO, GL_SRC_COLOR);

        //  Go back to viewport transform that coverts the [-1,1]x[-1,1]
        // normalized device coordinates space.
        glViewport(0, 0, GLsizei(width()), GLsizei(height()));        
        
        blur_1_.bind_as_texture();

#ifndef GEO_OS_EMSCRIPTEN       
        //   Since there is no more GL_LUMINANCE, our texture has GL_RED
        // texture storage. We can avoid writing a specialized shader by
        // using texture swizzle mask to copy the red comonent into the
        // other components (this emulates the old GL_LUMINANCE mode).
        static GLint swizzle_mask[] = {GL_RED, GL_RED, GL_RED, GL_RED};
        glTexParameteriv(
            GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask
        );
#endif
       
        draw_unit_textured_quad();
       
#ifndef GEO_OS_EMSCRIPTEN       
        static GLint no_swizzle_mask[] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
        glTexParameteriv(
            GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, no_swizzle_mask
        );
#endif       
        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        blur_1_.unbind();
    }

    void AmbientOcclusionImpl::compute_SSAO() {
        blur_1_.bind_as_framebuffer();
        glClearColor(1.0, 1.0, 1.0, 0.0);
        glClear(GL_COLOR_BUFFER_BIT);
        
        glDisable(GL_DEPTH_TEST);
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        
        glUseProgram(SSAO_program_);
        
        // 'false' for column major order
        glUniformMatrix4fv(proj_inv_loc_, 1, false, proj_inv_);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        
        glActiveTexture(GL_TEXTURE1);
        reduced_depth_FBO_.bind_as_texture();
        glActiveTexture(GL_TEXTURE0);
        
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, random_tex_);
        glViewport(0, 0, GLsizei(width()), GLsizei(height()));

        draw_unit_textured_quad();
	
        glActiveTexture(GL_TEXTURE2);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        glEnable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        
        blur_1_.unbind();

        glUseProgram(0);
    }

    void AmbientOcclusionImpl::blur() {
        if(blur_width_ < 1) {
            return;
        }
        glDisable(GL_DEPTH_TEST);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, depth_tex_);
        glActiveTexture(GL_TEXTURE0);
        
        // Horizontal blur: blur_1_ -> blur_2_
        blur_2_.bind_as_framebuffer();
        GLSL::set_program_uniform_by_name(blur_program_, "vertical", false);
        glUseProgram(blur_program_);
        blur_1_.bind_as_texture();
        draw_unit_textured_quad();
        blur_2_.unbind();
        
        // Vertical blur: blur_2_ -> blur_1_
        blur_1_.bind_as_framebuffer();
        GLSL::set_program_uniform_by_name(blur_program_, "vertical", true);  
        glUseProgram(blur_program_);
        blur_2_.bind_as_texture();
        draw_unit_textured_quad();
        blur_1_.unbind();
        glUseProgram(0);
        
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);

        glEnable(GL_DEPTH_TEST);
    }

    void AmbientOcclusionImpl::get_proj_inv() {
        GLfloat proj[16];
        glupGetMatrixfv(GLUP_PROJECTION_MATRIX, proj);
        if(!glupInvertMatrixfv(proj_inv_, proj)) {
            Logger::err("GLAmbientOcclusionDB")
                << "Projection matrix is singular" << std::endl;
        }
    }
    
}