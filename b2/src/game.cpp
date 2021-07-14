#include <random>

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include "bytebuffer.hpp"
#include "config.hpp"
#include "exception.hpp"
#include "game.hpp"
#include "isosurface.hpp"
#include "logger.hpp"
#include "timer.hpp"

namespace b2
{

const char *const Game::configPath = "configs/game.json";

Game::Game(const system::AssetManager &assetManager, const glm::ivec2 &surfaceSize)
	: assetManager(assetManager),
	  acceleration(glm::vec3(0.0f, -9.8f, 0.0f)),
	  alive(true),
	  singleThread(true),
	  projection(1.0f)
{
	using json = nlohmann::json;

	const Config config(assetManager.readFile(configPath));
	const json physicsConfig = config.json.at("physics");

	singleThread.store(config.json.at("singleThread").get<bool>());

	initLogicThread(
		surfaceSize, physicsConfig.at("gridSize").at("width").get<size_t>(),
		physicsConfig.at("particlesCount").get<size_t>());
	initRender(surfaceSize);
}

Game::~Game()
{
	alive.store(false);
	logicThread.join();
}

void Game::update()
{
	using namespace gl;

	presentScene();
}

void Game::onSensorsEvent(const glm::vec3 &acceleration)
{
	this->acceleration.push(acceleration);
}

void Game::initLogicThread(const glm::ivec2 &surfaceSize, size_t gridWidth, size_t particlesCount)
{
	_assert(gridWidth > 0, 0xd78eead8);
	_assert(particlesCount > 0, 0xd78eead8);

	gridSize = glm::ivec3(gridWidth, int32_t(gridWidth * float(surfaceSize.y) / surfaceSize.x), gridWidth);

	particlesCloud = physics::Cloud(gridSize, particlesCount, [this]() -> physics::Particle {
		std::default_random_engine generator(Timer::getTimestamp());
		std::uniform_real_distribution<float> x(0.5f, float(gridSize.x) - 0.5f), y(0.5f, float(gridSize.y) - 0.5f),
			z(0.5f, float(gridSize.z) - 0.5f);

		return physics::Particle(glm::vec3(x(generator), y(generator), z(generator)));
	});
	isosurface = Isosurface(gridSize + glm::ivec3(margin));
	logicThread = std::thread(logicRoutine, this);
}

void Game::initRender(const glm::ivec2 &surfaceSize)
{
	using namespace gl;

	const Shader vertexShader(ShaderType::Vertex, assetManager.readFile("shaders/surface.vs")),
		fragmentShader(ShaderType::Fragment, assetManager.readFile("shaders/surface.fs"));

	shaderProgram = ShaderProgram({vertexShader, fragmentShader});
	projection = camera.getPerspective(75.0f, float(surfaceSize.x) / surfaceSize.y, 1000.f);

	camera.lookAt(glm::vec3(.0f, 0.0f, -50.f), glm::vec3(0.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0));
}

void Game::presentScene()
{
	using namespace gl;

	const glm::vec3 boxSize(gridSize + glm::ivec3(margin));
	std::vector<Isosurface::MeshVertex> &localMesh = mesh.get();

	if (localMesh.size() == 0)
		return;

	if (!surfaceVertices || surfaceVertices.getSize() < localMesh.size() * sizeof(Isosurface::MeshVertex))
	{
		surfaceVertices = Buffer(BufferType::Vertex, localMesh);
	}
	else
	{
		surfaceVertices.bind();
		surfaceVertices.write(0, localMesh);
	}

	surfaceVertices.bind();
	setVertexFormat(std::vector<VertexAttrib>(
		{{3, sizeof(Isosurface::MeshVertex), AttribType::Float},
		 {3, sizeof(Isosurface::MeshVertex), AttribType::Float}}));

	shaderProgram.use();

	Mat4Uniform("in_projection", projection).set(shaderProgram);
	Mat4Uniform("in_modelview", camera.getView() * glm::translate(glm::mat4(1.f), -boxSize * 0.5f)).set(shaderProgram);

	_i(glEnable, GL_DEPTH_TEST);

	setClearColor({.5f, .6f, .4f, 1.f});
	clear(ClearMode::Color | ClearMode::Depth);
	draw(DrawMode::Triangles, localMesh.size());
}

void Game::logicRoutine(Game *self)
{
	size_t frames = 0;
	float elapsed = 0.0f, pTime = 0.0f, rTime = 0.0f;
	Timer globalTimer;

	while (self->alive.load())
	{
		/*
			Physics config:
			- particles count	:		4000
			- grid width		:		24
			- physics iters.	:		3
			- solver iters.		:		2

			ST: 9 ms
			MT: 7 ms
		*/
		Timer localTimer;
		bool singleThread = self->singleThread.load();

		self->particlesCloud.update(self->acceleration.get(), 0.01f, singleThread);
		pTime += localTimer.getDeltaMs();

		self->mesh.swap(self->isosurface.generateMesh(self->particlesCloud.getParticles(), radius, singleThread));

		rTime += localTimer.getDeltaMs();

		++frames;
		elapsed += globalTimer.getDeltaMs();

		if (elapsed >= 1000.0f)
		{
			info("Physics: %f, Render: %f", pTime / frames, rTime / frames);

			frames = 0;
			elapsed = pTime = rTime = 0.0f;
		}
	}
}

} // namespace b2
