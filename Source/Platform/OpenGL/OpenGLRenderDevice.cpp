/*  
 * Copyright (c) 2022-2023 Alexander Kaminsky and Maxwell Hunt
 *  
 * This file incorporates work covered by the following copyright and permission notice:  
 *  
 *     Copyright (c) 2018 Benny Bobaganoosh
 * 
 *     Permission is hereby granted, free of charge, to any person obtaining a copy
 *     of this software and associated documentation files (the "Software"), to deal
 *     in the Software without restriction, including without limitation the rights
 *     to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 *     copies of the Software, and to permit persons to whom the Software is
 *     furnished to do so, subject to the following conditions:
 *     
 *     The above copyright notice and this permission notice shall be included in all
 *     copies or substantial portions of the Software.
 *     
 *     THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 *     IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 *     FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 *     AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 *     LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 *     OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 *     SOFTWARE.
 */

#include "OpenGLRenderDevice.h"

#include <string>
#include <cstring> // std::memcpy
#include <vector>
#include <unordered_map>
#include <iostream>

/**
 * @brief Compiles and adds an OpenGL shader to an OpenGL shader program.
 * @param shaderProgram Target shader program ID.
 * @param text Shader text to compile. This is typically the 'source code' of a shader file.
 * @param type Shader type (for example, vertex shader, fragment shader, etc.).
 * @param shaders Vector to add the newly created shader ID to.
 * @return true if the shader was compiled and added successfully.
 */
static bool AddShader(GLuint shaderProgram, const std::string& text, GLenum type, 
	std::vector<GLuint>* shaders);

/**
 * @brief Adds and binds all attributes in a shader program. Note: this assumes attributes are
 *		listed in order, which isn't true for all compilers.
 * @param program Target shader program ID.
 * @param version Current OpenGL version. If the OpenGL version is above 320 (3.2), layout is used
 *		in favor of attributes.
 */
static void AddAllAttributes(GLuint program, unsigned int version);

/**
 * @brief Polls OpenGL for shader errors.
 * @param shader ID of the shader to check errors for.
 * @param flag Specifies which parameter to check for errors. For more details see documentation for
 *		glGetProgramiv and glGetShaderiv.
 * @param isProgram Whether shader is a program object or a shader object.
 * @param errorMessage Error message prefixed to the OpenGL error message.
 * @return true if there was an error.
 */
static bool CheckShaderError(GLuint shader, int flag, bool isProgram, 
	const std::string& errorMessage);

/**
 * @brief Fetches all uniform blocks and uniform variables from an OpenGL shader.
 * @note Non-sampler2D uniforms are currently unsupported.
 * @param shaderProgram Target shader program ID.
 * @param uniformMap Map to push uniform block names and locations to, for future lookup.
 * @param samplerMap Map to push sampler2D uniform names and locations to, for future lookup.
 */
static void AddShaderUniforms(GLuint shaderProgram, 
	std::unordered_map<std::string, GLint>& uniformMap,
	std::unordered_map<std::string, GLint>& samplerMap);

bool OpenGLRenderDevice::isInitialized = false;

bool OpenGLRenderDevice::GlobalInit()
{
	// Already initialized
	if (isInitialized) return true;

	// OpenGL Version 3.3
	constexpr int major = 3;
	constexpr int minor = 3;

	isInitialized = true;
	// Attempt to set core profile
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE) != 0)
	{
		std::cerr << "Warning: Could not set core OpenGL profile" << std::endl;
		isInitialized = false;
	}
	// Attempt to set major version
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major) != 0)
	{
		std::cerr << "Error: Could not set major OpenGL version to " << major << ": "
			<< SDL_GetError() << std::endl;
		isInitialized = false;
	}
	// Attempt to set minor version
	if (SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor) != 0)
	{
		std::cerr << "Error: Could not set minor OpenGL version to " << minor << ": "
			<< SDL_GetError() << std::endl;
		isInitialized = false;
	}

	return isInitialized;
}

OpenGLRenderDevice::OpenGLRenderDevice(Window& window) : 
	shaderVersion(""), 
	version(0),
	boundFBO(0),
	viewportFBO(0),
	boundVAO(0),
	boundShader(0),
	currentFaceCulling(FACE_CULL_NONE),
	currentDepthFunc(DRAW_FUNC_ALWAYS),
	currentSourceBlend(BLEND_FUNC_NONE),
	currentDestBlend(BLEND_FUNC_NONE),
	currentStencilFunc(DRAW_FUNC_ALWAYS),
	currentStencilTestMask((unsigned int)0xFFFFFFFF),
	currentStencilWriteMask((unsigned int)0xFFFFFFFF),
	currentStencilComparisonVal(0),
	currentStencilFail(STENCIL_KEEP),
	currentStencilPassButDepthFail(STENCIL_KEEP),
	currentStencilPass(STENCIL_KEEP),
	blendingEnabled(false),
	shouldWriteDepth(false),
	stencilTestEnabled(false),
	scissorTestEnabled(false),
	currentPackAlignment(0),
	currentUnpackAlignment(0)
{
	// Create OpenGL context in the target window
	context = SDL_GL_CreateContext(window.GetWindowHandle());
	
	// Initialize GLEW, check if failed
	const GLenum result = glewInit();
	if (result != GLEW_OK)
	{
		std::cerr << "Error: " << glewGetErrorString(result) << std::endl;
		throw std::runtime_error("Render device failed to initialize.");
	}

	// Set framebuffer (which contains color, depth, stencil, etc. buffers) to specified size
	FBOData fboWindowData;
	fboWindowData.width = window.GetWidth();
	fboWindowData.height = window.GetHeight();
	fboMap[0] = fboWindowData;

	glEnable(GL_DEPTH_TEST); // Ensures we have a depth buffer (for details look up Z-buffering)
	glDepthFunc(DRAW_FUNC_ALWAYS); // Default the depth buffer to always pass; we will set later
	glDepthMask(GL_FALSE); // Default to disabling depth buffer write; this can change later
	glFrontFace(GL_CCW); // Specifies which side front-facing based on vertex winding order
}

OpenGLRenderDevice::~OpenGLRenderDevice()
{
	// Cleanup
	SDL_GL_DeleteContext(context);
}

unsigned int OpenGLRenderDevice::CreateRenderTarget(unsigned int texture, unsigned int width, 
	unsigned int height, FramebufferAttachment attachment, unsigned int attachmentNumber, 
	unsigned int mipLevel)
{
	// Create a framebuffer object (FBO)
	unsigned int fbo;
	glGenBuffers(1, &fbo);
	SetFBO(fbo);

	const GLenum attachmentTypeGL = attachment + attachmentNumber;
	glFramebufferTexture2D(GL_FRAMEBUFFER, attachmentTypeGL, GL_TEXTURE_2D, texture, mipLevel);

	// Save framebuffer width and height
	FBOData data;
	data.width = width;
	data.height = height;
	fboMap[fbo] = data;

	return fbo;
}

void OpenGLRenderDevice::UpdateRenderTarget(unsigned int fbo, unsigned int width, 
	unsigned int height)
{
	// Update the width and height of the framebuffer (viewport size set in SetViewport)
	fboMap[0].width = width;
	fboMap[0].height = height;
}

unsigned int OpenGLRenderDevice::ReleaseRenderTarget(unsigned int fbo)
{
	// Default framebuffer; should not be deleted.
	if (fbo == 0) return 0;

	// Check if the framebuffer exists...
	const std::unordered_map<unsigned int, FBOData>::iterator it = fboMap.find(fbo);

	// Framebuffer could not be found; it was never created or was already deleted.
	if (it == fboMap.end())
	{
		return 0;
	}

	// Delete framebuffer and associated data...
	glDeleteFramebuffers(1, &fbo);
	fboMap.erase(it);
	return 0;
}

unsigned int OpenGLRenderDevice::CreateVertexArray(const float** vertexData, 
	const unsigned int* vertexElementSizes, unsigned int numVertexComponents, 
	unsigned int numInstanceComponents, unsigned int numVertices, const unsigned int* indices, 
	unsigned int numIndices, BufferUsage usage)
{
	// Vertex Components + Instance Components + Indices
	const unsigned int numBuffers = numVertexComponents + numInstanceComponents + 1;

	GLuint vao; // Vertex Array Object (VAO)
	GLuint* buffers = new GLuint[numBuffers];
	size_t* bufferSizes = new size_t[numBuffers];

	// Generate 1 vertex array
	glGenVertexArrays(1, &vao);
	SetVAO(vao);

	glGenBuffers(numBuffers, buffers);
	for (unsigned int i = 0, attribute = 0; i < numBuffers - 1; i++)
	{
		BufferUsage attributeUsage = usage;
		bool inInstancedMode = false;

		// If this is an instance component
		if (i >= numVertexComponents)
		{
			attributeUsage = USAGE_DYNAMIC_DRAW; // Instanced data will be written each frame
			inInstancedMode = true;
		}

		const unsigned int elementSize = vertexElementSizes[i];
		// If this is an instance component, there is no data to bind as it will be updated each
		// frame. If no vertex data was supplied, leave it empty.
		const void* bufferData = (inInstancedMode || vertexData == nullptr) ? nullptr : vertexData[i];
		const size_t dataSize = inInstancedMode 
			? elementSize * sizeof(float) 
			: elementSize * sizeof(float) * numVertices;

		glBindBuffer(GL_ARRAY_BUFFER, buffers[i]);
		glBufferData(GL_ARRAY_BUFFER, dataSize, bufferData, attributeUsage);
		bufferSizes[i] = dataSize;

		// Because OpenGL doesn't support attributes with more than 4 elements, each set of 4 
		// elements gets its own attribute.
		const unsigned int elementSizeDiv = elementSize / 4;
		const unsigned int elementSizeRem = elementSize % 4;
		for (unsigned int j = 0; j < elementSizeDiv; j++)
		{
			glEnableVertexAttribArray(attribute);
			// Specify how to interpret vertex buffer data
			glVertexAttribPointer(attribute, 4, GL_FLOAT, GL_FALSE, elementSize * sizeof(GLfloat),
				(const GLvoid*)(sizeof(GLfloat) * j * 4));

			// If this is an instanced attribute...
			if (inInstancedMode)
			{
				// Setting the attribute divisor to 1 tells OpenGL to update the content of the
				// vertex attribute when we start to render a new instance. By default, this is set
				// to 0 which means the contents of the vertex attribute are updated each iteration
				// of the vertex shader.
				glVertexAttribDivisor(attribute, 1);
			}

			attribute++;
		}
		if (elementSizeRem != 0) // Last remaining set of less than 4 elements
		{
			glEnableVertexAttribArray(attribute);
			// Specify how to interpret vertex buffer data
			glVertexAttribPointer(attribute, elementSizeRem, GL_FLOAT, GL_FALSE, 
				elementSize * sizeof(GLfloat),
				(const GLvoid*)(sizeof(GLfloat) * elementSizeDiv * 4));

			// If this is an instanced attribute...
			if (inInstancedMode)
			{
				// Setting the attribute divisor to 1 tells OpenGL to update the content of the
				// vertex attribute when we start to render a new instance. By default, this is set
				// to 0 which means the contents of the vertex attribute are updated each iteration
				// of the vertex shader.
				glVertexAttribDivisor(attribute, 1);
			}

			attribute++;
		}
	}

	// Bind vertex array indices...
	const size_t indicesSize = numIndices * sizeof(unsigned int);
	glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffers[numBuffers - 1]);
	glBufferData(GL_ELEMENT_ARRAY_BUFFER, indicesSize, indices, usage);

	bufferSizes[numBuffers - 1] = indicesSize;

	VertexArray vaoData;
	vaoData.buffers = buffers;
	vaoData.bufferSizes = bufferSizes;
	vaoData.numBuffers = numBuffers;
	vaoData.numElements = numIndices;
	vaoData.usage = usage;
	vaoData.instanceComponentsStartIndex = numVertexComponents;
	vaoMap[vao] = vaoData;

	return vao;
}

void OpenGLRenderDevice::UpdateVertexArrayBuffer(unsigned int vao, unsigned int bufferIndex, 
	const void* data, size_t dataSize)
{
	// Vertex Array Object (VAO) 0 is null. No functions that modify VAO state should be called.
	if (vao == 0)
	{
		return;
	}

	// Check if the VAO exists...
	const std::unordered_map<unsigned int, VertexArray>::iterator it = vaoMap.find(vao);

	// VAO could not be found; it was never created or was deleted.
	if (it == vaoMap.end())
	{
		return;
	}

	const VertexArray* vaoData = &it->second;
	BufferUsage usage;
	// If we are modifying a per-instance component, set it to dynamic draw (hint to GPU)
	if (bufferIndex >= vaoData->instanceComponentsStartIndex)
	{
		usage = USAGE_DYNAMIC_DRAW;
	}
	else
	{
		usage = vaoData->usage;
	}

	SetVAO(vao);
	glBindBuffer(GL_ARRAY_BUFFER, vaoData->buffers[bufferIndex]);
	if (vaoData->bufferSizes[bufferIndex] >= dataSize)
	{
		glBufferSubData(GL_ARRAY_BUFFER, 0, dataSize, data);
	}
	else // More memory needs to be allocated for this buffer
	{
		glBufferData(GL_ARRAY_BUFFER, dataSize, data, usage);
		vaoData->bufferSizes[bufferIndex] = dataSize;
	}
}

unsigned int OpenGLRenderDevice::ReleaseVertexArray(unsigned int vao)
{
	// Vertex Array Object (VAO) 0 is null. No functions that modify VAO state should be called.
	if (vao == 0)
	{
		return 0;
	}
	
	// Check if the VAO exists...
	const std::unordered_map<unsigned int, VertexArray>::iterator it = vaoMap.find(vao);

	// VAO could not be found; it was never created or was already deleted.
	if (it == vaoMap.end())
	{
		return 0;
	}

	// Delete the VAO...
	const VertexArray* vaoData = &it->second;
	glDeleteVertexArrays(1, &vao);
	glDeleteBuffers(vaoData->numBuffers, vaoData->buffers);
	delete[] vaoData->buffers;
	delete[] vaoData->bufferSizes;
	vaoMap.erase(it);

	return 0;
}

unsigned int OpenGLRenderDevice::CreateSampler(SamplerFilter minFilter, SamplerFilter magFilter, 
	SamplerWrapMode wrapU, SamplerWrapMode wrapV, float anisotropy)
{
	unsigned int result = 0;
	glGenSamplers(1, &result);
	glSamplerParameteri(result, GL_TEXTURE_WRAP_S, wrapU);
	glSamplerParameteri(result, GL_TEXTURE_WRAP_T, wrapV);
	glSamplerParameteri(result, GL_TEXTURE_MAG_FILTER, magFilter);
	glSamplerParameteri(result, GL_TEXTURE_MIN_FILTER, minFilter);
	if (anisotropy != 0.0f && minFilter != FILTER_NEAREST && minFilter != FILTER_LINEAR)
	{
		glSamplerParameterf(result, GL_TEXTURE_MAX_ANISOTROPY_EXT, anisotropy);
	}
	return result;
}

unsigned int OpenGLRenderDevice::ReleaseSampler(unsigned int sampler)
{
	// Sampler 0 is null, nothing to delete.
	if (sampler == 0)
	{
		return 0;
	}

	glDeleteSamplers(1, &sampler);
	return 0;
}

static GLint GetOpenGLFormat(OpenGLRenderDevice::PixelFormat format)
{
	switch (format)
	{
	case OpenGLRenderDevice::FORMAT_R: return GL_RED;
	case OpenGLRenderDevice::FORMAT_RG: return GL_RG;
	case OpenGLRenderDevice::FORMAT_RGB: return GL_RGB;
	case OpenGLRenderDevice::FORMAT_RGBA: return GL_RGBA;
	case OpenGLRenderDevice::FORMAT_DEPTH: return GL_DEPTH_COMPONENT;
	case OpenGLRenderDevice::FORMAT_DEPTH_AND_STENCIL: return GL_DEPTH_STENCIL;
	default:
		std::cerr << "Error: PixelFormat " << format << " is not a valid PixelFormat." << std::endl;
		return 0;
	}
}

static GLint GetOpenGLInternalFormat(OpenGLRenderDevice::PixelFormat format, bool compress)
{
	switch (format)
	{
	case OpenGLRenderDevice::FORMAT_R: return GL_RED;
	case OpenGLRenderDevice::FORMAT_RG: return GL_RG;
	case OpenGLRenderDevice::FORMAT_RGB:
		if (compress)
		{
			return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
		}
		return GL_RGB;
	case OpenGLRenderDevice::FORMAT_RGBA:
		if (compress)
		{
			return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
		}
		return GL_RGBA;
	case OpenGLRenderDevice::FORMAT_DEPTH: return GL_DEPTH_COMPONENT;
	case OpenGLRenderDevice::FORMAT_DEPTH_AND_STENCIL: return GL_DEPTH_STENCIL;
	default:
		std::cerr << "Error: PixelFormat " << format << " is not a valid PixelFormat." << std::endl;
		return 0;
	}
}

unsigned int OpenGLRenderDevice::CreateTexture2D(int width, int height, const void* data, 
	PixelFormat dataFormat, PixelFormat internalFormat, bool generateMipmaps, bool compress,
	int packAlignment, int unpackAlignment)
{
	const GLint format = GetOpenGLFormat(dataFormat);
	const GLint glInternalFormat = GetOpenGLInternalFormat(internalFormat, compress);
	const GLenum textureTarget = GL_TEXTURE_2D;
	GLuint textureHandle;

	if (packAlignment != currentPackAlignment)
	{
		// This specifies the alignment requirements for the start of each pixel row in memory. 
		glPixelStorei(GL_PACK_ALIGNMENT, packAlignment);
		currentPackAlignment = packAlignment;
	}

	if (unpackAlignment != currentUnpackAlignment)
	{
		// This specifies the alignment requirements for the start of each pixel row in memory. 
		glPixelStorei(GL_UNPACK_ALIGNMENT, unpackAlignment);
		currentUnpackAlignment = unpackAlignment;
	}

	glGenTextures(1, &textureHandle);
	glBindTexture(textureTarget, textureHandle);
	glTexParameterf(textureTarget, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameterf(textureTarget, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(textureTarget, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(textureTarget, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glTexImage2D(textureTarget, 0, glInternalFormat, width, height, 0, format, GL_UNSIGNED_BYTE, 
		data);

	if (generateMipmaps)
	{
		glGenerateMipmap(textureTarget);
	}
	else
	{
		glTexParameteri(textureTarget, GL_TEXTURE_BASE_LEVEL, 0);
		glTexParameteri(textureTarget, GL_TEXTURE_MAX_LEVEL, 0);
	}

	return textureHandle;
}

unsigned int OpenGLRenderDevice::ReleaseTexture2D(unsigned int texture2D)
{
	// Texture 0 is null, nothing to delete.
	if (texture2D == 0)
	{
		return 0;
	}

	glDeleteTextures(1, &texture2D);
	return 0;
}

unsigned int OpenGLRenderDevice::CreateUniformBuffer(const void* data, size_t dataSize, 
	BufferUsage usage)
{
	unsigned int ubo;
	glGenBuffers(1, &ubo);
	glBindBuffer(GL_UNIFORM_BUFFER, ubo);
	glBufferData(GL_UNIFORM_BUFFER, dataSize, data, usage);
	return ubo;
}

void OpenGLRenderDevice::UpdateUniformBuffer(unsigned int buffer, const void* data, size_t dataSize)
{
	glBindBuffer(GL_UNIFORM_BUFFER, buffer);
	void* destination = glMapBuffer(GL_UNIFORM_BUFFER, GL_WRITE_ONLY);
	std::memcpy(destination, data, dataSize);
	glUnmapBuffer(GL_UNIFORM_BUFFER);
}

unsigned int OpenGLRenderDevice::ReleaseUniformBuffer(unsigned int buffer)
{
	// Uniform buffer 0 is null, nothing to delete.
	if (buffer == 0)
	{
		return 0;
	}

	glDeleteBuffers(1, &buffer);
	return 0;
}

unsigned int OpenGLRenderDevice::CreateShaderProgram(const std::string& shaderText)
{
	const GLuint shaderProgram = glCreateProgram();

	// Should never be 0 as shader 0 is null. Something went wrong...
	if (shaderProgram == 0)
	{
		std::cerr << "Error creating shader program." << std::endl;
		return (unsigned int)-1;
	}

	// #version ... must come before anything else; we will insert the define after it.
	size_t defineInsertPosition = shaderText.find("\n", shaderText.find("#version"));

	// We did not find #version. This is likely because the shader is missing #version; error.
	if (defineInsertPosition == std::string::npos)
	{
		std::cerr << "Error: Shader program is missing #version directive." << std::endl;
		return (unsigned int)-1;
	}

	defineInsertPosition++; // Set the position after the newline

	std::string vertexShaderText = shaderText;
	std::string fragmentShaderText = shaderText;

	vertexShaderText.insert(defineInsertPosition, "#define VERTEX_SHADER_BUILD\n");
	fragmentShaderText.insert(defineInsertPosition, "#define FRAGMENT_SHADER_BUILD\n");

	ShaderProgram programData;
	if (!AddShader(shaderProgram, vertexShaderText, GL_VERTEX_SHADER, &programData.shaders))
	{
		return (unsigned int)-1;
	}
	if (!AddShader(shaderProgram, fragmentShaderText, GL_FRAGMENT_SHADER, &programData.shaders))
	{
		return (unsigned int)-1;
	}

	glLinkProgram(shaderProgram);
	if (CheckShaderError(shaderProgram, GL_LINK_STATUS, true, "Error linking shader program"))
	{
		return (unsigned int)-1;
	}

	glValidateProgram(shaderProgram);
	if (CheckShaderError(shaderProgram, GL_VALIDATE_STATUS,	true, "Invalid shader program"))
	{
		return (unsigned int)-1;
	}

	AddAllAttributes(shaderProgram, GetVersion());
	AddShaderUniforms(shaderProgram, programData.uniformMap, programData.samplerMap);

	shaderProgramMap[shaderProgram] = programData;
	return shaderProgram;
}

void OpenGLRenderDevice::SetShaderUniformBuffer(unsigned int shader, 
	const std::string& uniformBufferName, unsigned int buffer)
{
	SetShader(shader);
	glBindBufferBase(GL_UNIFORM_BUFFER,	shaderProgramMap[shader].uniformMap[uniformBufferName],
		buffer);
}

void OpenGLRenderDevice::SetShaderSampler(unsigned int shader, const std::string& samplerName, 
	unsigned int texture, unsigned int sampler, unsigned int unit)
{
	SetShader(shader);
	glActiveTexture(GL_TEXTURE0 + unit);
	glBindTexture(GL_TEXTURE_2D, texture);
	glBindSampler(unit, sampler);
	glUniform1i(shaderProgramMap[shader].samplerMap[samplerName], unit);
}

unsigned int OpenGLRenderDevice::ReleaseShaderProgram(unsigned int shader)
{
	// Shader program 0 is null, nothing to delete.
	if (shader == 0) return 0;

	// Check if the shader exists...
	const auto programIt = shaderProgramMap.find(shader);
	if (programIt == shaderProgramMap.end())
	{
		// Shader could not be found, it was never created or was already deleted.
		return 0;
	}

	const ShaderProgram* shaderProgram = &programIt->second;

	// Delete all attached shaders
	for (std::vector<unsigned int>::const_iterator it = shaderProgram->shaders.begin();
		it != shaderProgram->shaders.end(); ++it)
	{
		glDetachShader(shader, *it);
		glDeleteShader(*it);
	}
	glDeleteProgram(shader);
	shaderProgramMap.erase(programIt);
	return 0;
}

void OpenGLRenderDevice::SetShaderInt(unsigned int shader, const std::string& name, int value)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform1i(location, value);
}

void OpenGLRenderDevice::SetShaderIntArray(unsigned int shader, const std::string& name, 
	int* values, uint32_t count)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform1iv(location, count, values);
}

void OpenGLRenderDevice::SetShaderFloat(unsigned int shader, const std::string& name, float value)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform1f(location, value);
}

void OpenGLRenderDevice::SetShaderFloat2(unsigned int shader, const std::string& name, 
	const float* values)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform2f(location, values[0], values[1]);
}

void OpenGLRenderDevice::SetShaderFloat3(unsigned int shader, const std::string& name, 
	const float* values)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform3f(location, values[0], values[1], values[2]);
}

void OpenGLRenderDevice::SetShaderFloat4(unsigned int shader, const std::string& name, 
	const float* values)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniform4f(location, values[0], values[1], values[2], values[3]);
}

void OpenGLRenderDevice::SetShaderMat3(unsigned int shader, const std::string& name, 
	const float* values)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniformMatrix3fv(location, 1, GL_FALSE, values);
}

void OpenGLRenderDevice::SetShaderMat4(unsigned int shader, const std::string& name, 
	const float* values)
{
	SetShader(shader);
	const GLint location = glGetUniformLocation(shader, name.c_str());
	glUniformMatrix4fv(location, 1, GL_FALSE, values);
}

void OpenGLRenderDevice::Clear(unsigned int fbo, bool shouldClearColor, bool shouldClearDepth, 
	bool shouldClearStencil, const float r, const float g, const float b, const float a, 
	unsigned int stencil)
{
	SetFBO(fbo);
	unsigned int flags = 0;

	if (shouldClearColor)
	{
		flags |= GL_COLOR_BUFFER_BIT;
		glClearColor(r, g, b, a);
	}

	if (shouldClearDepth)
	{
		flags |= GL_DEPTH_BUFFER_BIT;
	}

	if (shouldClearStencil)
	{
		flags |= GL_STENCIL_BUFFER_BIT;
		SetStencilWriteMask(stencil);
	}

	glClear(flags);
}

void OpenGLRenderDevice::Draw(unsigned int fbo, unsigned int shader, unsigned int vao, 
	const DrawParameters& drawParameters, unsigned int numInstances, unsigned int numElements)
{
	// Nothing to draw...
	if (numInstances == 0)
	{
		return;
	}

	// Note: Ensure correct drawing process order
	SetFBO(fbo);
	SetViewport(fbo);
	SetDrawParameters(drawParameters);
	SetShader(shader);
	SetVAO(vao);

	if (numInstances == 1)
	{
		glDrawElements(drawParameters.primitiveType, (GLsizei)numElements, GL_UNSIGNED_INT, 0);
	}
	else
	{
		glDrawElementsInstanced(drawParameters.primitiveType, (GLsizei)numElements, GL_UNSIGNED_INT, 
			0, numInstances);
	}
}

void OpenGLRenderDevice::SetDrawParameters(const OpenGLRenderDevice::DrawParameters& drawParameters)
{
	SetBlending(drawParameters.sourceBlend, drawParameters.destBlend);
	SetScissorTest(drawParameters.useScissorTest, drawParameters.scissorStartX,
		drawParameters.scissorStartY, drawParameters.scissorWidth, drawParameters.scissorHeight);
	SetFaceCulling(drawParameters.faceCulling);
	SetDepthTest(drawParameters.shouldWriteDepth, drawParameters.depthFunc);
}

void OpenGLRenderDevice::SetFBO(unsigned int fbo)
{
	// If the specified framebuffer object (FBO) is already bound, no change is needed.
	if (fbo == boundFBO)
	{
		return;
	}

	// Bind the FBO and save the currently bound FBO. We can reduce glBindFramebuffer calls by
	// checking what the currently bound FBO is and avoid binding the same FBO repeatedly.
	glBindFramebuffer(GL_FRAMEBUFFER, fbo);
	boundFBO = fbo;
}

void OpenGLRenderDevice::SetViewport(unsigned int fbo)
{
	const FBOData& fboData = fboMap[fbo];

	// If the viewport is already using the specified framebuffer object (FBO), and the viewport
	// size does not need to be updated, no change is needed.
	if (fbo == viewportFBO && fboData.width == viewportWidth && fboData.height == viewportHeight)
	{
		return;
	}

	glViewport(0, 0, fboData.width, fboData.height);
	viewportFBO = fbo;
	viewportWidth = fboData.width;
	viewportHeight = fboData.height;
}

void OpenGLRenderDevice::SetVAO(unsigned int vao)
{
	// If the specified vertex array object (VAO) is already bound, no change is needed.
	if (vao == boundVAO)
	{
		return;
	}

	// Bind the VAO and save the currently bound VAO. We can reduce glBindVertexArray calls by
	// checking what the currently bound VAO is and avoid binding the same VAO repeatedly.
	glBindVertexArray(vao);
	boundVAO = vao;
}

void OpenGLRenderDevice::SetShader(unsigned int shader)
{
	// If the specified shader is already bound, no change is needed.
	if (shader == boundShader)
	{
		return;
	}

	// Bind the shader and save the currently bound shader. We can reduce glUseProgram calls by
	// checking what the currently bound shader is and avoid binding the same shader repeatedly.
	glUseProgram(shader);
	boundShader = shader;
}

void OpenGLRenderDevice::SetFaceCulling(FaceCulling faceCulling)
{
	// If the specified face culling setting is already set, no change is needed.
	if (faceCulling == currentFaceCulling)
	{
		return;
	}

	// Face culling is enabled, but needs to be disabled...
	if (faceCulling == FACE_CULL_NONE)
	{ 
		glDisable(GL_CULL_FACE);
	}
	// Face culling is disabled but needs to be enabled...
	else if (currentFaceCulling == FACE_CULL_NONE)
	{ 
		glEnable(GL_CULL_FACE);
		glCullFace(faceCulling);
	}
	// Only need to change culling state...
	else
	{
		glCullFace(faceCulling);
	}

	currentFaceCulling = faceCulling;
}

void OpenGLRenderDevice::SetDepthTest(bool shouldWrite, DrawFunc depthFunc)
{
	if (shouldWrite != shouldWriteDepth)
	{
		glDepthMask(shouldWrite ? GL_TRUE : GL_FALSE);
		shouldWriteDepth = shouldWrite;
	}

	// If the specified depth test function is already set, no change is needed.
	if (depthFunc == currentDepthFunc)
	{
		return;
	}

	// Set the depth test function and save it. We can reduce glDepthFunc calls by checking what the 
	// currently set depth test function is.
	glDepthFunc(depthFunc);
	currentDepthFunc = depthFunc;
}

void OpenGLRenderDevice::SetBlending(BlendFunc sourceBlend, BlendFunc destinationBlend)
{
	// If the specified blend functions are already set, no change is needed.
	if (sourceBlend == currentSourceBlend && destinationBlend == currentDestBlend)
	{
		return;
	}
	// No blending...
	else if (sourceBlend == BLEND_FUNC_NONE || destinationBlend == BLEND_FUNC_NONE)
	{
		glDisable(GL_BLEND);
	}
	// Blending was previously disabled, but now needs to be enabled.
	else if (currentSourceBlend == BLEND_FUNC_NONE || currentDestBlend == BLEND_FUNC_NONE)
	{
		glEnable(GL_BLEND);
		glBlendFunc(sourceBlend, destinationBlend);
	}
	// Blending is already enabled, set the new blend functions.
	else
	{
		glBlendFunc(sourceBlend, destinationBlend);
	}

	// We can reduce glDisable, glEnable, and glBlendFunc calls by checking what the currently set 
	// blend function is and avoid setting the same blend functions repeatedly.
	currentSourceBlend = sourceBlend;
	currentDestBlend = destinationBlend;
}

void OpenGLRenderDevice::SetStencilTest(bool enable, DrawFunc stencilFunc, 
	unsigned int stencilTestMask, unsigned int stencilWriteMask, unsigned int stencilComparisonVal, 
	StencilOp stencilFail, StencilOp stencilPassButDepthFail, StencilOp stencilPass)
{
	if (enable != stencilTestEnabled)
	{
		// If we want to enable stencil testing and it is not already enabled
		if (enable)
		{
			glEnable(GL_STENCIL_TEST);
		}
		// If we want to disable stencil testing and it is not already disabled
		else
		{
			glDisable(GL_STENCIL_TEST);
		}
		// Avoid enabling or disabling repeatedly by saving the current state.
		stencilTestEnabled = enable;
	}

	// Check if the currently set stencil functions match the specified stencil functions. If they
	// already match, we do not need to repeatedly set them.
	if (stencilFunc != currentStencilFunc || stencilTestMask != currentStencilTestMask
		|| stencilComparisonVal != currentStencilComparisonVal)
	{
		glStencilFunc(stencilFunc, stencilTestMask, stencilComparisonVal);
		currentStencilComparisonVal = stencilComparisonVal;
		currentStencilTestMask = stencilTestMask;
		currentStencilFunc = stencilFunc;
	}

	// Check if the currently set stencil test behavior matches the specified stencil test behavior. 
	// If they already match, we do not need to repeatedly set them.
	if (stencilFail != currentStencilFail || stencilPass != currentStencilPass
		|| stencilPassButDepthFail != currentStencilPassButDepthFail)
	{
		glStencilOp(stencilFail, stencilPassButDepthFail, stencilPass);
		currentStencilFail = stencilFail;
		currentStencilPass = stencilPass;
		currentStencilPassButDepthFail = stencilPassButDepthFail;
	}

	SetStencilWriteMask(stencilWriteMask);
}

void OpenGLRenderDevice::SetStencilWriteMask(unsigned int mask)
{
	// If the stencil write mask is already set, no change is needed.
	if (currentStencilWriteMask == mask)
	{
		return;
	}

	glStencilMask(mask);
	currentStencilWriteMask = mask;
}

void OpenGLRenderDevice::SetScissorTest(bool enable, unsigned int startX, unsigned int startY, 
	unsigned int width, unsigned int height)
{
	// If we want to disable scissor testing
	if (!enable)
	{
		// If scissor testing is already disabled, no change is needed.
		if (!scissorTestEnabled)
		{
			return;
		}
		// Disable...
		else
		{
			glDisable(GL_SCISSOR_TEST);
			scissorTestEnabled = false;
			return;
		}
	}

	if (!scissorTestEnabled)
	{
		glEnable(GL_SCISSOR_TEST);
	}

	glScissor(startX, startY, width, height);
	scissorTestEnabled = true;
}

unsigned int OpenGLRenderDevice::GetVersion()
{
	if (version != 0) // If we already found the version
	{
		return version;
	}

	int majorVersion;
	int minorVersion;

	glGetIntegerv(GL_MAJOR_VERSION, &majorVersion);
	glGetIntegerv(GL_MINOR_VERSION, &minorVersion);

	// For example, if major version is 3 and minor version is 3, then the OpenGL version is 330
	version = (unsigned int)(majorVersion * 100 + minorVersion * 10);
	return version;
}

std::string OpenGLRenderDevice::GetShaderVersion()
{
	if (!shaderVersion.empty())
	{
		return shaderVersion;
	}

	const unsigned int version = GetVersion();

	if (version >= 330)
	{
		shaderVersion = std::to_string(version);
	}
	else if (version >= 320)
	{
		shaderVersion = "150";
	}
	else if (version >= 310)
	{
		shaderVersion = "140";
	}
	else if (version >= 300)
	{
		shaderVersion = "130";
	}
	else if (version >= 210)
	{
		shaderVersion = "120";
	}
	else if (version >= 200)
	{
		shaderVersion = "110";
	}
	else
	{
		const int majorVersion = version / 100;
		const int minorVersion = (version / 10) % 10;
		std::cerr << "Error: OpenGL Version " << majorVersion << "." << minorVersion <<
			"does not support shaders." << std::endl;
		return "";
	}

	return shaderVersion;
}

bool AddShader(GLuint shaderProgram, const std::string& text, GLenum type, 
	std::vector<GLuint>* shaders)
{
	const GLuint shader = glCreateShader(type);

	// Should never be 0 as shader 0 is null. Something went wrong...
	if (shader == 0)
	{
		std::cerr << "Error creating shader type " << type << std::endl;
		return false;
	}

	// Convert string into GLchar*
	const GLchar* p[1];
	p[0] = text.c_str();

	// Save string length as GLint
	GLint lengths[1];
	lengths[0] = (GLint)text.length();

	glShaderSource(shader, 1, p, lengths);
	glCompileShader(shader);

	GLint success;
	glGetShaderiv(shader, GL_COMPILE_STATUS, &success);
	if (!success)
	{
		GLchar infoLog[1024]; // Error message string will always be 1024 in length

		glGetShaderInfoLog(shader, 1024, NULL, infoLog);
		std::cerr << "Error compiling shader type " << shader << ": '" << infoLog << " '"
			<< std::endl;
		return false;
	}

	glAttachShader(shaderProgram, shader);
	shaders->push_back(shader);
	return true;
}

void AddAllAttributes(GLuint program, unsigned int version)
{
	if (version >= 320)
	{
		// Layout is enabled; return.
		return;
	}

	// TODO: This code assumes attributes are listed in order, which isn't true for all compilers.
	// It's safe to ignore for now because OpenGL versions requiring this aren't being used.
	GLint numActiveAttributes = 0;
	GLint maxAttribNameLength = 0;

	glGetProgramiv(program, GL_ACTIVE_ATTRIBUTES, &numActiveAttributes);
	glGetProgramiv(program, GL_ACTIVE_ATTRIBUTE_MAX_LENGTH, &maxAttribNameLength);

	std::vector<GLchar> nameData(maxAttribNameLength);
	for (GLint attribute = 0; attribute < numActiveAttributes; ++attribute)
	{
		GLint arraySize = 0;
		GLenum type = 0;
		GLsizei actualLength = 0;

		glGetActiveAttrib(program, attribute, nameData.size(), &actualLength, &arraySize, &type, 
			&nameData[0]);

		glBindAttribLocation(program, attribute, (char*)&nameData[0]);
	}
}

bool CheckShaderError(GLuint shader, int flag, bool isProgram, const std::string& errorMessage)
{
	GLint success = 0;
	GLchar error[1024] = { 0 };

	if (isProgram)
	{
		glGetProgramiv(shader, flag, &success);
	}
	else
	{
		glGetShaderiv(shader, flag, &success);
	}

	if (!success)
	{
		if (isProgram)
		{
			glGetProgramInfoLog(shader, sizeof(error), NULL, error);
		}
		else
		{
			glGetShaderInfoLog(shader, sizeof(error), NULL, error);
		}

		std::cerr << errorMessage << ": " << error << std::endl;
		return true;
	}
	return false;
}

void AddShaderUniforms(GLuint shaderProgram, std::unordered_map<std::string, GLint>& uniformMap, 
	std::unordered_map<std::string, GLint>& samplerMap)
{
	// Get the number of active uniform blocks for the program 
	GLint numBlocks;
	glGetProgramiv(shaderProgram, GL_ACTIVE_UNIFORM_BLOCKS, &numBlocks);
	for (int block = 0; block < numBlocks; ++block)
	{
		GLint nameLength;
		glGetActiveUniformBlockiv(shaderProgram, block,	GL_UNIFORM_BLOCK_NAME_LENGTH, &nameLength);

		std::vector<GLchar> name(nameLength);
		glGetActiveUniformBlockName(shaderProgram, block, nameLength, NULL, &name[0]);
		std::string uniformBlockName((char*)&name[0], nameLength - 1);
		// Save the uniform block index so that we can easily lookup the index of a given block.
		uniformMap[uniformBlockName] = glGetUniformBlockIndex(shaderProgram, &name[0]);
	}

	// Get the number of active uniform variables for the program 
	const GLint numUniforms = 0;
	glGetProgramiv(shaderProgram, GL_ACTIVE_UNIFORMS, &numBlocks);

	// Would get GL_ACTIVE_UNIFORM_MAX_LENGTH, but buggy on some drivers.
	std::vector<GLchar> uniformName(256);
	for (int uniform = 0; uniform < numUniforms; ++uniform)
	{
		GLint arraySize = 0;
		GLenum type = 0;
		GLsizei actualLength = 0;
		glGetActiveUniform(shaderProgram, uniform, uniformName.size(),
			&actualLength, &arraySize, &type, &uniformName[0]);
		if (type != GL_SAMPLER_2D)
		{
			std::cerr << "Error: Non-sampler2D uniforms currently unsupported!" << std::endl;
			continue;
		}
		std::string name((char*)&uniformName[0], actualLength - 1);
		// Since only sampler2D uniforms are supported, save it to our sampler map so that we can
		// easily look up the uniform variable index of a sampler.
		samplerMap[name] = glGetUniformLocation(shaderProgram, (char*)&uniformName[0]);
	}
}
